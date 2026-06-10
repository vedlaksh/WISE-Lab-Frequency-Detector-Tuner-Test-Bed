#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "yin.h"   /* FFT-accelerated YIN pitch detection (also pulls in kiss_fft.h) */

#define CHANNELS       2
#define ACTIVE_CHANNEL 0   /* try 1 if your L/R pin selects the other channel */

/* One analysis block of raw 48 kHz mono samples, plus the interleaved stereo
 * staging buffer ALSA reads into. YIN_RAW_LEN (8192) raw samples decimate to
 * YIN_BUF (1024) samples @ 6 kHz. */
static int16_t analysis_buf[YIN_RAW_LEN];
static int32_t pcm_buf[YIN_RAW_LEN * CHANNELS];

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

    unsigned int rate = YIN_FS;
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

/* Read one YIN_RAW_LEN block of frames and extract the active channel as int16. */
static int read_audio_block_i16(int16_t *out)
{
    int frames_read_total = 0;

    while (frames_read_total < YIN_RAW_LEN) {
        int frames_needed = YIN_RAW_LEN - frames_read_total;

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

    for (int i = 0; i < YIN_RAW_LEN; i++) {
        int32_t s32 = pcm_buf[i * CHANNELS + ACTIVE_CHANNEL];

        /*
         * INMP441 gives 24-bit-ish audio in a 32-bit frame.
         * Shift down to fit into int16_t. If signal is too small, try >> 12;
         * if it clips, try >> 18 or >> 20.
         */
        out[i] = (int16_t)(s32 >> 16);
    }

    return 0;
}

int main(void)
{
    if (alsa_capture_init() < 0) {
        return 1;
    }

    if (yin_init() != 0) {
        printf("yin init failed\n");
        return 1;
    }

    while (1) {
        read_audio_block_i16(analysis_buf);

        float conf = 0.0f;
        int freq = yin_detect(analysis_buf, &conf);

        if (freq > 0) {
            printf("freq=%d Hz (conf=%.2f, lvl=%.4f)\n", freq, conf, yin_level);
        } else {
            printf("no signal (conf=%.2f, lvl=%.4f)\n", conf, yin_level);
        }
    }

    snd_pcm_close(pcm);
    return 0;
}
