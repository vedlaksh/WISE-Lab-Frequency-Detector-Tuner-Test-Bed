#include "shim.h"          /* host imports (host_read_sample, host_printf, host_should_continue, ...) */
#include <stdint.h>
#include <stdarg.h>

#include "yin.h"           /* FFT-accelerated YIN pitch detection (also pulls in kiss_fft.h) */

/* Safety cap on recorded blocks; the primary stop is host_should_continue()
 * going to 0 when you press Ctrl-C on the host. Because that return value is
 * recorded, replay stops at the identical iteration. */
#define MAX_BLOCKS 1000000L

static int16_t analysis_buf[YIN_RAW_LEN];

/* Minimal printf supporting %s, %d, %ld -> forwarded to the host as ptr+len. */
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

/* Pull one analysis block (YIN_RAW_LEN raw 48 kHz samples) from the host,
 * one sample per host call. Each return value is recorded by record-replay. */
static void read_audio_block_i16(int16_t *out)
{
    for (int i = 0; i < YIN_RAW_LEN; i++) {
        out[i] = (int16_t)host_read_sample();
    }
}

int run(void)
{
    if (host_alsa_capture_init() < 0) {
        wasm_printf("alsa init failed\n");
        return 1;
    }

    if (yin_init() != 0) {
        wasm_printf("yin init failed\n");
        return -1;
    }

    for (long k = 0; k < MAX_BLOCKS && host_should_continue(); k++) {
        read_audio_block_i16(analysis_buf);

        float conf = 0.0f;
        int freq = yin_detect(analysis_buf, &conf);
        int conf_pct = (int)(conf * 100.0f + 0.5f);

        if (freq > 0) {
            wasm_printf("freq=%d Hz (conf=%d%)\n", freq, conf_pct);
        } else {
            wasm_printf("no signal (conf=%d%)\n", conf_pct);
        }
    }

    host_snd_pcm_close();
    return 0;
}
