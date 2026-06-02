// #include <alsa/asoundlib.h>
#include "shim.h"
// #include "../wit_gen/host.h"
// #include <math.h>
#include <stdint.h>
#include <stdarg.h>
// #include <stdio.h>
// #include <stdlib.h>

#include "kiss_fft.h"

#define N 512
#define SAMPLE_RATE 48000
#define CHANNELS 2
#define ACTIVE_CHANNEL 0   // try 1 if your L/R pin selects the other channel

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t analysis_buf[N];

static kiss_fft_cpx fft_in[N];
static kiss_fft_cpx fft_out[N];
static float hann_window[N];

static char fft_mem[8192];
static kiss_fft_cfg fft_cfg;

int32_t pcm_buf[N * CHANNELS];

char print_buf[64];

static void wasm_printf(const char *fmt, ...) {
    static char buf[96];
    int i = 0;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && i < 94) {
        if (fmt[0] == '%') {
            if (fmt[1] == 's') {
                const char *s = va_arg(ap, const char *);
                while (*s && i < 94) buf[i++] = *s++;
                fmt += 2;
            } else if (fmt[1] == 'd' || (fmt[1] == 'l' && fmt[2] == 'd')) {
                long d = (fmt[1] == 'l') ? va_arg(ap, long) : va_arg(ap, int);
                if (fmt[1] == 'l') fmt += 3; else fmt += 2;
                if (d < 0) { buf[i++] = '-'; d = -d; }
                char tmp[12]; int ti = 0;
                if (d == 0) { tmp[ti++] = '0'; }
                while (d > 0) { tmp[ti++] = '0' + (d % 10); d /= 10; }
                while (ti-- > 0 && i < 94) buf[i++] = tmp[ti];
            } else {
                buf[i++] = *fmt++;
            }
        } else {
            buf[i++] = *fmt++;
        }
    }
    va_end(ap);
    buf[i] = '\0';

    host_printf(buf, (unsigned int)i);
}

static int detect_best_freq_from_i16(const int16_t *samples, int sample_rate)
{
    if (!samples || sample_rate <= 0 || !fft_cfg) return 0;

    float mean = 0.0f;
    for (int i = 0; i < N; i++) {
        mean += (float)samples[i];
    }
    mean /= (float)N;

    for (int i = 0; i < N; i++) {
        fft_in[i].r = ((float)samples[i] - mean) * hann_window[i];
        fft_in[i].i = 0.0f;
    }

    kiss_fft(fft_cfg, fft_in, fft_out);

    int min_bin = (int)(20.0f * (float)N / (float)sample_rate);
    int max_bin = (int)(2000.0f * (float)N / (float)sample_rate);

    if (min_bin < 1) min_bin = 1;
    if (max_bin > (N / 2) - 2) max_bin = (N / 2) - 2;

    int peak_bin = -1;
    float peak_mag = 0.0f;

    for (int i = min_bin; i <= max_bin; i++) {
        float re = fft_out[i].r;
        float im = fft_out[i].i;
        float mag = re * re + im * im;

        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = i;
        }
    }

    if (peak_bin < 0) return 0;

    return peak_bin * sample_rate / N;
}


static int read_audio_block_i16(int16_t *out)
{
    for (int i = 0; i < N; i++) {
        out[i] = (int16_t)host_read_sample();
    }

    return 0;
}

int run(void)
{
    if (host_alsa_capture_init() < 0) {
        return 1;
    }

    size_t fft_len = sizeof(fft_mem);
    fft_cfg = kiss_fft_alloc(N, 0, fft_mem, &fft_len);
    if (!fft_cfg) {
        // printf("fft alloc failed\n");
        // int len = snprintf(print_buf, sizeof(print_buf), "fft alloc failed\n");
        // host_log((uint32_t)print_buf, len);
        wasm_printf("fft alloc failed\n");
        return -1;
    }

    for (int i = 0; i < N; i++) {
        hann_window[i] =
            0.5f * (1.0f - (float)host_cos(2.0f * M_PI * (float)i / (float)(N - 1)));
    }

    while (1) {
        // read_audio_block_i16(analysis_buf, N);

        // if(read_audio_block_i16(analysis_buf) != 0){
        //     wasm_printf("Error Reading From Audio\n");
        // }
        read_audio_block_i16(analysis_buf);

        int freq = detect_best_freq_from_i16(analysis_buf, SAMPLE_RATE);

        if (freq > 0) {
            // printf("freq=%d Hz\n", freq);
            // int len = snprintf(print_buf, sizeof(print_buf), "freq=%d\n", freq);
            // rpi_host_log((uint32_t)print_buf, len);
            wasm_printf("freq=%d Hz\n", freq);
        } else {
            // printf("no signal\n");
            // int len = snprintf(print_buf, sizeof(print_buf), "no signal\n");
            // rpi_host_log((uint32_t)print_buf, len);
            wasm_printf("no signal\n");
        }
    }

    host_snd_pcm_close();
    return 0;
}
