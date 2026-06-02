#include <stddef.h>
#include <stdint.h>

#define WASM_IMPORT(name) __attribute__((import_module("rpi"), import_name(#name)))


WASM_IMPORT(host-alsa-capture-init)
extern int host_alsa_capture_init();

// WASM_IMPORT(host-read-audio-block-i16)
// extern int host_read_audio_block_i16(uint32_t n);

WASM_IMPORT(host-read-sample)
extern int host_read_sample();

WASM_IMPORT(host-snd-pcm-close)
extern void host_snd_pcm_close();

WASM_IMPORT(host-printf)
extern void host_printf(const char *ptr, unsigned int len);

WASM_IMPORT(host-sin)
extern double host_sin(double x);

WASM_IMPORT(host-cos)
extern double host_cos(double x);