#include "shim.h"

/* ── Trig: delegate to host (wasm has no native sin/cos) ── */

double sin(double x) { return host_sin(x); }
double cos(double x) { return host_cos(x); }
float cosf(float x) { return (float)host_cos((double)x); }

// dummy stubs to satisfy linker
void *malloc(size_t s) { (void)s; return 0; }
void free(void *p) { (void)p; }
