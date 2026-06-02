#include <alsa/asoundlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N 512
#define SAMPLE_RATE 48000
#define CHANNELS 2
#define ACTIVE_CHANNEL 0   // try 1 if your L/R pin selects the other channel

snd_pcm_t *pcm = NULL;

int host_alsa_capture_init(void)
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

int32_t host_read_sample(void)
{
    int32_t frame[CHANNELS];

    while (1) {
        int err = snd_pcm_readi(pcm, frame, 1);  // read 1 frame

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

        if (err == 1) {
            int32_t s32 = frame[ACTIVE_CHANNEL];
            return (int16_t)(s32 >> 16);
        }
    }
}

// int host_read_audio_block_i16(int16_t *out, int n)
// {
//     int frames_read_total = 0;

//     while (frames_read_total < n) {
//         int frames_needed = n - frames_read_total;

        
//         int err = snd_pcm_readi(
//             pcm,
//             &pcm_buf[frames_read_total * CHANNELS],
//             frames_needed
//         );

//         if (err == -EPIPE) {
//             fprintf(stderr, "overrun\n");
//             snd_pcm_prepare(pcm);
//             continue;
//         }

//         if (err < 0) {
//             fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror(err));
//             snd_pcm_prepare(pcm);
//             continue;
//         }

//         frames_read_total += err;
//     }

//     for (int i = 0; i < n; i++) {
//         int32_t s32 = pcm_buf[i * CHANNELS + ACTIVE_CHANNEL];

//         /*
//          * INMP441 gives 24-bit-ish audio in a 32-bit frame.
//          * Shift down to fit into int16_t for your existing FFT path.
//          *
//          * If signal is too small, try >> 12 instead of >> 16.
//          * If it clips, try >> 18 or >> 20.
//          */
//         out[i] = (int16_t)(s32 >> 16);
//     }

//     return 0;
// }

void host_snd_pcm_close(void){
    snd_pcm_close(pcm);
}

void host_printf(const char *ptr, size_t len){
    printf("%.*s", (int)len, ptr);
    return;
}

double host_sin(double x) { return sin(x); }
double host_cos(double x) { return cos(x); }