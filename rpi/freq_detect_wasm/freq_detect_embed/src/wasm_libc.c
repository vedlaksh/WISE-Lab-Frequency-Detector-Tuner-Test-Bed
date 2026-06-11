#include "shim.h"

/* ── Trig: delegate to host (wasm has no native sin/cos) ── */

double sin(double x) { return host_sin(x); }
double cos(double x) { return host_cos(x); }
float cosf(float x) { return (float)host_cos((double)x); }

// dummy stubs to satisfy linker
void *malloc(size_t s) { (void)s; return 0; }
void free(void *p) { (void)p; }

/* ── Bump allocator for the component-model canonical ABI ──
 * host-read-block returns list<s16>; the ABI lowers it into guest memory via
 * the exported cabi_realloc. We serve that from a static arena that the guest
 * resets (wasm_heap_reset) before each block, so it never actually grows. */
static unsigned char wasm_heap[65536];
static size_t        wasm_heap_off = 0;

void wasm_heap_reset(void) { wasm_heap_off = 0; }

__attribute__((export_name("cabi_realloc")))
void *cabi_realloc(void *ptr, size_t old_size, size_t align, size_t new_size) {
    (void)ptr; (void)old_size;
    if (new_size == 0) return (void *)(uintptr_t)align;
    size_t off = (wasm_heap_off + (align - 1)) & ~(align - 1);
    if (off + new_size > sizeof(wasm_heap)) return 0;   /* OOM */
    wasm_heap_off = off + new_size;
    return &wasm_heap[off];
}
