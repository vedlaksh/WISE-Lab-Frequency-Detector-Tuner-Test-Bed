#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kiss_fft.h"

#define N 512
#define SAMPLE_RATE 48000
#define CHANNELS 2
#define ACTIVE_CHANNEL 0   // try 1 if your L/R pin selects the other channel

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t analysis_buf[N];
static int32_t pcm_buf[N * CHANNELS];

static kiss_fft_cpx fft_in[N];
static kiss_fft_cpx fft_out[N];
static float hann_window[N];

static char fft_mem[8192];
static kiss_fft_cfg fft_cfg;

static snd_pcm_t *pcm = NULL;

static int alsa_capture_init(void)
{
    snd_pcm_hw_params_t *params;
    int err;

    err = snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open failed: %s\n", snd_strerror(err));
        return err;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);

    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL);

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return err;
    }

    printf("ALSA capture initialized, rate=%u\n", rate);
    return 0;
}

static int read_audio_block_i16(int16_t *out)
{
    int frames_read_total = 0;

    while (frames_read_total < N) {
        int frames_needed = N - frames_read_total;

        int err = snd_pcm_readi(
            pcm,
            &pcm_buf[frames_read_total * CHANNELS],
            frames_needed
        );

        if (err == -EPIPE) {
            fprintf(stderr, "overrun\n");
            snd_pcm_prepare(pcm);
            continue;
        }

        if (err < 0) {
            fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror(err));
            snd_pcm_prepare(pcm);
            continue;
        }

        frames_read_total += err;
    }

    for (int i = 0; i < N; i++) {
        int32_t s32 = pcm_buf[i * CHANNELS + ACTIVE_CHANNEL];

        /*
         * INMP441 gives 24-bit-ish audio in a 32-bit frame.
         * Shift down to fit into int16_t for your existing FFT path.
         *
         * If signal is too small, try >> 12 instead of >> 16.
         * If it clips, try >> 18 or >> 20.
         */
        out[i] = (int16_t)(s32 >> 16);
    }

    return 0;
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

int main(void)
{
    if (alsa_capture_init() < 0) {
        return 1;
    }

    size_t fft_len = sizeof(fft_mem);
    fft_cfg = kiss_fft_alloc(N, 0, fft_mem, &fft_len);
    if (!fft_cfg) {
        printf("fft alloc failed\n");
        return 1;
    }

    for (int i = 0; i < N; i++) {
        hann_window[i] =
            0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(N - 1)));
    }

    while (1) {
        read_audio_block_i16(analysis_buf);

        int freq = detect_best_freq_from_i16(analysis_buf, SAMPLE_RATE);

        if (freq > 0) {
            printf("freq=%d Hz\n", freq);
        } else {
            printf("no signal\n");
        }
    }

    snd_pcm_close(pcm);
    return 0;
}
