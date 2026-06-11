#include <stddef.h>
#include <stdint.h>

#define WASM_IMPORT(name) __attribute__((import_module("rpi"), import_name(#name)))


WASM_IMPORT(host-alsa-capture-init)
extern int host_alsa_capture_init();

/* component-model list<s16> return: {ptr, len} written into guest linear memory */
typedef struct { int16_t *ptr; size_t len; } host_list_s16_t;

WASM_IMPORT(host-read-block)
extern void host_read_block(uint32_t n, host_list_s16_t *ret);

WASM_IMPORT(host-snd-pcm-close)
extern void host_snd_pcm_close();

WASM_IMPORT(host-printf)
extern void host_printf(const char *ptr, unsigned int len);

WASM_IMPORT(host-sin)
extern double host_sin(double x);

WASM_IMPORT(host-cos)
extern double host_cos(double x);

WASM_IMPORT(host-should-continue)
extern int host_should_continue(void);