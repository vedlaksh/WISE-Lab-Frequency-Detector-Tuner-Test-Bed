/*
 * yin.h - FFT-accelerated YIN pitch detection (header-only, no malloc).
 *
 * Shared verbatim by the native baseline (freq_detect_test) and the WASM guest
 * (freq_detect_embed). It depends only on kiss_fft and the C library functions
 * sin()/cos() (double precision). In the WASM guest those route to the host via
 * wasm_libc.c; natively they come from libm. There is no dynamic allocation:
 * all scratch lives in static buffers and the two kiss_fft cfgs are placed in
 * static arenas (matching the project's existing no-malloc design).
 *
 * Why YIN instead of FFT peak-picking: a 512-pt FFT at 48 kHz has 93.75 Hz bins
 * and no interpolation, so it cannot resolve (or even separate) the low musical
 * fundamentals and frequently locks onto a harmonic. YIN works in the lag/time
 * domain, targets the FUNDAMENTAL directly, and with parabolic interpolation
 * reaches sub-Hz accuracy. It also yields an aperiodicity/confidence value for a
 * principled "no signal" decision.
 *
 * Pipeline per analysis block (YIN_RAW_LEN int16 samples @ YIN_FS):
 *   1. anti-alias FIR low-pass + decimate by YIN_D  -> YIN_BUF samples @ YIN_FS_DEC
 *      (decimation keeps the FFT/lag sizes small while still reaching 20 Hz)
 *   2. remove DC (mean) and scale to ~[-1,1] for FFT numerical conditioning
 *   3. YIN difference function d(tau), tau in [0, YIN_TAU_MAX]:
 *         d(tau) = e0 + e(tau) - 2*r(tau)
 *      e0     = sum_{j=0}^{W-1} x[j]^2                  (constant)
 *      e(tau) = sum_{j=0}^{W-1} x[j+tau]^2             (sliding window energy)
 *      r(tau) = sum_{j=0}^{W-1} x[j]*x[j+tau]          (windowed cross-correlation)
 *      r(tau) is obtained from kiss_fft via Wiener-Khinchin on a (the W-sample
 *      integration window) and b (the whole buffer):
 *         r = IFFT( conj(FFT(a)) .* FFT(b) ) / NFFT
 *      Zero-padding a and b to NFFT >= W + YIN_TAU_MAX makes the lags [0,TAU_MAX]
 *      free of circular wraparound, so r(tau) is the exact linear correlation.
 *   4. cumulative mean normalized difference function d'(tau)
 *   5. absolute-threshold pick: first local min below YIN_THRESHOLD, else argmin
 *   6. parabolic interpolation of tau  ->  f0 = YIN_FS_DEC / tau
 *   7. confidence = 1 - d'(tau*); reject if below YIN_CONF_MIN
 */
#ifndef YIN_H
#define YIN_H

#include <stdint.h>
#include <stddef.h>
#include "kiss_fft.h"

/* ---- tunable parameters ---- */
#define YIN_FS         48000                    /* native ALSA sample rate         */
#define YIN_D          8                        /* decimation factor               */
#define YIN_FS_DEC     (YIN_FS / YIN_D)         /* 6000 Hz decimated rate          */
#define YIN_BUF        1024                     /* decimated samples per block     */
#define YIN_RAW_LEN    (YIN_BUF * YIN_D)        /* 8192 raw samples per block      */
#define YIN_TAU_MAX    320                      /* min freq 6000/320 = 18.75 Hz    */
#define YIN_TAU_MIN    3                        /* max freq 6000/3   = 2000 Hz     */
#define YIN_WIN        (YIN_BUF - YIN_TAU_MAX)  /* integration window = 704 samples*/
#define YIN_NFFT       2048                     /* > YIN_WIN+YIN_TAU_MAX, pow2     */
#define YIN_THRESHOLD  0.12f                    /* YIN absolute threshold          */
#define YIN_CONF_MIN   0.50f                    /* min confidence to report a pitch*/
#define YIN_RMS_MIN    0.010f                   /* silence gate: reject blocks whose
                                                 * in-band RMS is below this. TUNE to
                                                 * your mic: watch the printed lvl in
                                                 * silence vs a tone and set between. */
#define YIN_FIR_TAPS   129                      /* anti-alias FIR length (odd)     */
#define YIN_FIR_FC     2600.0                   /* FIR cutoff (Hz); < YIN_FS_DEC/2 */
#define YIN_FIR_C2     32768.0f                 /* input scale (int16 -> ~[-1,1])  */

/* kiss_fft arena: needs sizeof(kiss_fft_state)+sizeof(kiss_fft_cpx)*(nfft-1)
 * ~= 16.7 KB for NFFT=2048. 24 KB is a safe over-allocation. */
#define YIN_FFT_ARENA  24576

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- static state (only the single TU that includes this gets a copy) ---- */
static float        yin_fir[YIN_FIR_TAPS];
static float        yin_buf[YIN_BUF];
static kiss_fft_cpx yin_ta[YIN_NFFT];     /* FFT input / product / scratch */
static kiss_fft_cpx yin_tb[YIN_NFFT];     /* FFT input / IFFT output       */
static kiss_fft_cpx yin_fa[YIN_NFFT];     /* spectrum A                    */
static kiss_fft_cpx yin_fb[YIN_NFFT];     /* spectrum B                    */
static char         yin_fwd_mem[YIN_FFT_ARENA];
static char         yin_inv_mem[YIN_FFT_ARENA];
static kiss_fft_cfg yin_fwd;
static kiss_fft_cfg yin_inv;
static float        yin_dp[YIN_TAU_MAX + 1];   /* CMNDF */
static float        yin_level;                 /* RMS of the last analyzed block (for tuning) */

/* Hamming-windowed sinc low-pass, normalized to unity DC gain. */
static void yin_design_fir(void)
{
    const int    M  = YIN_FIR_TAPS - 1;            /* even */
    const double fc = YIN_FIR_FC / (double)YIN_FS; /* normalized cutoff (0..0.5) */
    double sum = 0.0;
    for (int n = 0; n < YIN_FIR_TAPS; n++) {
        double m = (double)n - (double)M / 2.0;
        /* __builtin_* so no math.h prototype is needed in the -nostdlib wasm guest;
         * lowers to libm natively and routes to host_sin/host_cos in the guest. */
        double sinc = (m == 0.0) ? (2.0 * fc)
                                 : (__builtin_sin(2.0 * M_PI * fc * m) / (M_PI * m));
        double w = 0.54 - 0.46 * __builtin_cos(2.0 * M_PI * (double)n / (double)M); /* Hamming */
        double h = sinc * w;
        yin_fir[n] = (float)h;
        sum += h;
    }
    if (sum != 0.0) {
        for (int n = 0; n < YIN_FIR_TAPS; n++)
            yin_fir[n] = (float)((double)yin_fir[n] / sum);
    }
}

/* Allocate the two kiss_fft cfgs and design the FIR. Returns 0 on success. */
static int yin_init(void)
{
    size_t lf = sizeof(yin_fwd_mem);
    size_t li = sizeof(yin_inv_mem);
    yin_fwd = kiss_fft_alloc(YIN_NFFT, 0, yin_fwd_mem, &lf);
    yin_inv = kiss_fft_alloc(YIN_NFFT, 1, yin_inv_mem, &li);
    if (!yin_fwd || !yin_inv) return -1;
    yin_design_fir();
    return 0;
}

/* Anti-alias FIR + decimate raw[YIN_RAW_LEN] (int16 @ YIN_FS) into
 * yin_buf[YIN_BUF] (@ YIN_FS_DEC), scaled to ~[-1,1]. Edges are index-clamped. */
static void yin_decimate(const int16_t *raw)
{
    const int M2 = YIN_FIR_TAPS / 2;   /* filter center offset */
    for (int n = 0; n < YIN_BUF; n++) {
        int center = n * YIN_D;
        float acc = 0.0f;
        for (int k = 0; k < YIN_FIR_TAPS; k++) {
            int idx = center + k - M2;
            if (idx < 0) idx = 0;
            else if (idx >= YIN_RAW_LEN) idx = YIN_RAW_LEN - 1;
            acc += yin_fir[k] * (float)raw[idx];
        }
        yin_buf[n] = acc / YIN_FIR_C2;
    }
}

/* Detect the fundamental. Returns f0 in Hz (rounded) or 0 if no confident pitch.
 * If out_conf != NULL it receives the confidence in [0,1]. */
static int yin_detect(const int16_t *raw, float *out_conf)
{
    if (out_conf) *out_conf = 0.0f;
    if (!yin_fwd || !yin_inv) return 0;

    /* 1+2: decimate, then remove DC mean */
    yin_decimate(raw);
    double mean = 0.0;
    for (int i = 0; i < YIN_BUF; i++) mean += yin_buf[i];
    mean /= (double)YIN_BUF;
    for (int i = 0; i < YIN_BUF; i++) yin_buf[i] -= (float)mean;

    /* 2b: silence gate. Compute the in-band RMS and reject quiet blocks before
     * doing any FFT work, so ambient mic noise/rumble is not reported as a pitch. */
    double ms = 0.0;
    for (int i = 0; i < YIN_BUF; i++) { double v = yin_buf[i]; ms += v * v; }
    ms /= (double)YIN_BUF;
    yin_level = (float)__builtin_sqrt(ms);
    if (ms < (double)YIN_RMS_MIN * (double)YIN_RMS_MIN) {
        return 0;   /* below the noise floor -> no signal (out_conf already 0) */
    }

    /* 3a: windowed cross-correlation r(tau) via FFT.
     *     a = yin_buf[0..YIN_WIN-1] (zero-padded), b = yin_buf[0..YIN_BUF-1] */
    for (int i = 0; i < YIN_NFFT; i++) {
        yin_ta[i].r = (i < YIN_WIN) ? yin_buf[i] : 0.0f; yin_ta[i].i = 0.0f;
        yin_tb[i].r = (i < YIN_BUF) ? yin_buf[i] : 0.0f; yin_tb[i].i = 0.0f;
    }
    kiss_fft(yin_fwd, yin_ta, yin_fa);   /* A = FFT(a) */
    kiss_fft(yin_fwd, yin_tb, yin_fb);   /* B = FFT(b) */
    for (int k = 0; k < YIN_NFFT; k++) { /* conj(A) .* B */
        float ar = yin_fa[k].r, ai = yin_fa[k].i;
        float br = yin_fb[k].r, bi = yin_fb[k].i;
        yin_ta[k].r = ar * br + ai * bi;
        yin_ta[k].i = ar * bi - ai * br;
    }
    kiss_fft(yin_inv, yin_ta, yin_tb);   /* yin_tb = IFFT(...) (unnormalized) */

    /* 3b: e0 (constant integration-window energy) */
    double e0 = 0.0;
    for (int j = 0; j < YIN_WIN; j++) { double v = yin_buf[j]; e0 += v * v; }

    /* 4: difference function d(tau) and CMNDF d'(tau) */
    double e_tau   = e0;     /* e(0) == e0 */
    double running = 0.0;
    yin_dp[0] = 1.0f;
    int   best_tau = 0;
    float best_dp  = 1e30f;

    for (int tau = 1; tau <= YIN_TAU_MAX; tau++) {
        /* slide energy window: drop x[tau-1], add x[tau+YIN_WIN-1] */
        double drop = yin_buf[tau - 1];
        double add  = yin_buf[tau + YIN_WIN - 1];
        e_tau += add * add - drop * drop;

        double r = (double)yin_tb[tau].r / (double)YIN_NFFT;
        double d = e0 + e_tau - 2.0 * r;
        if (d < 0.0) d = 0.0;            /* numerical guard */

        running += d;
        double dp = (running > 0.0) ? (d * (double)tau / running) : 1.0;
        yin_dp[tau] = (float)dp;

        if (tau >= YIN_TAU_MIN && (float)dp < best_dp) { best_dp = (float)dp; best_tau = tau; }
    }

    /* 5: absolute threshold - first local min below threshold, else global min */
    int chosen = 0;
    for (int tau = YIN_TAU_MIN; tau < YIN_TAU_MAX; tau++) {
        if (yin_dp[tau] < YIN_THRESHOLD) {
            while (tau + 1 <= YIN_TAU_MAX && yin_dp[tau + 1] < yin_dp[tau]) tau++;
            chosen = tau;
            break;
        }
    }
    if (chosen == 0) chosen = best_tau;
    if (chosen < YIN_TAU_MIN) return 0;

    /* 6: parabolic interpolation around the chosen lag */
    float tau_f = (float)chosen;
    if (chosen > YIN_TAU_MIN && chosen < YIN_TAU_MAX) {
        float a = yin_dp[chosen - 1];
        float b = yin_dp[chosen];
        float c = yin_dp[chosen + 1];
        float denom = a + c - 2.0f * b;
        if (denom != 0.0f) {
            float shift = 0.5f * (a - c) / denom;
            if (shift > -1.0f && shift < 1.0f) tau_f += shift;
        }
    }

    /* 7: confidence + report */
    float conf = 1.0f - yin_dp[chosen];
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 1.0f) conf = 1.0f;
    if (out_conf) *out_conf = conf;

    if (conf < YIN_CONF_MIN) return 0;
    if (tau_f <= 0.0f) return 0;

    return (int)((float)YIN_FS_DEC / tau_f + 0.5f);
}

#endif /* YIN_H */
