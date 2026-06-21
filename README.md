# WALI Working Group Lab Frequency Detector — Tuner Test Bed

A Raspberry Pi test bed for **record-replay of WebAssembly** on embedded hardware, exercised with a real DSP workload: an **FFT-accelerated YIN pitch detector** reading a live I2S microphone.

The same pitch detector (`yin.h`) runs two ways:

- **Native** (`rpi/freq_detect_test/`) — plain C + ALSA, no WebAssembly. The baseline for validating the algorithm on hardware.
- **WASM + record-replay** (`rpi/freq_detect_wasm/`) — the detector compiled to a WebAssembly **component**, run on the Pi through a forked Wasmtime (`wasmtime-rr-prototyping`) that **records every host call** into a trace. That trace can then be **deterministically replayed on any machine**, with no microphone — capture-on-device, analyze-anywhere.

**All commands live in [`RUNBOOK.md`](RUNBOOK.md)**

---

## 1. Hardware

INMP441 I2S MEMS microphone → Raspberry Pi (Zero 2 W used here):

| INMP441 | Pi pin |
|---|---|
| VDD | 3V3 (pin 1) |
| GND | GND (pin 6) |
| L/R | GND (selects the **left** channel = `ACTIVE_CHANNEL 0`) |
| SCK | GPIO18 (pin 12) |
| WS  | GPIO19 (pin 35) |
| SD  | GPIO20 (pin 38) |

The mic must appear as an ALSA capture device (`dtparam=i2s=on` + `dtoverlay=googlevoicehat-soundcard` in `/boot/firmware/config.txt`, reboot, verify with `arecord`) — exact commands in **RUNBOOK A1**. If the card isn't `hw:0,0`, update that string in `freq_detect_test/src/main.c` and `rpi_embed/src/bridge.c`; if the signal's on the wrong channel, flip `ACTIVE_CHANNEL` (0 ⇄ 1).

---

## 2. Native tuner (baseline)

Builds directly on the Pi (clang + ALSA, no cross-compile) — commands in **RUNBOOK A2**. Output: `freq=<Hz> Hz (conf=<0..1>, lvl=<rms>)`, or `no signal`.

### Tuning to your mic (in `src/yin.h` / `src/main.c`)
- `YIN_RMS_MIN` (`yin.h`) — silence gate. Watch the printed `lvl` in silence vs. a tone and set it between them (default `0.010`).
- `s32 >> 16` (`main.c`) — input gain. Too quiet → try `>> 14`; clipping → `>> 18`.
- `YIN_CONF_MIN`, `YIN_THRESHOLD` (`yin.h`) — pitch confidence gates.

---

## 3. WASM + record-replay

The host (`rpi_embed`) is welded to the vendored Wasmtime fork (relative path dep), so it builds with the fork present. The Pi Zero is too weak to compile the fork, so build elsewhere. **On an Apple Silicon Mac this is easy: a Linux container is the same architecture as the Pi (aarch64), so we build natively — no cross-compile, no sysroot.** Match the container's distro to the Pi's so glibc lines up (this Pi runs **Ubuntu 24.04 / glibc 2.39**).

The build → deploy → replay pipeline (exact commands in **RUNBOOK Part A–B**):

1. **Build env** — an aarch64 Linux container on the Mac. Install the Rust nightly toolchain + `wasm-tools`, clone the repo. (RUNBOOK A3)
2. **Guest → component** — `clang --target=wasm32` builds `tuner.wasm`; `wasm-tools component embed`/`new` wrap it into `tuner.component.wasm` against `adc.wit`. (RUNBOOK A4)
3. **Host `rpi_embed`** — `cargo build` against the fork (native when `RPI_SYSROOT` is unset; `build.rs` cross-compiles only when it's set). (RUNBOOK A5)
4. **Deploy + record** — `docker cp` out, `scp` to the Pi, run; records to `./tuner.trace` (or `RPI_TRACE=/path`). (RUNBOOK A6, B1)
5. **Replay anywhere** — build the CLI with `--features rr` (the `replay` subcommand is feature-gated), then `wasmtime replay --trace tuner.trace …`. Replay restores recorded host-return values without re-running the host, so the `freq=` prints don't reappear. (RUNBOOK A5, B2)

### Live network streaming (record on the Pi → replay in the container, no file copy)

This repository also supports live network streaming. Instead of recording to a file and copying it over, point both ends at a **FIFO** and bridge the two FIFOs with `socat` over TCP, so the container replays the trace **as the Pi produces it**:
```
Pi:  rpi_embed ─write→ /tmp/tuner.fifo ─socat→ TCP:9000 ─socat→ /tmp/tuner.fifo ─read→ wasmtime replay  :container
```
No code changes are needed — the recorder writes whatever `RPI_TRACE` names and `replay --trace` reads whatever path it's given, both as plain sequential streams; the in-band `Eof` marker (written on Ctrl-C) ends replay cleanly. The trace arrives in bursts because the recorder batches events before flushing (`event_window_size`, default 16 — `wasm-crimp`); lowering that window tightens streaming latency at the cost of smaller, more frequent writes. Full setup, the live-stream run, the "prove it's live" stall test, and troubleshooting: **RUNBOOK Part C**.

---

## How record-replay works here

Every value crossing from the outside world into the WASM enters through one host-call boundary (`host_read_block`, `host_should_continue`, …). Recording writes each host-call **return value** into the trace; replay re-runs the same WASM but, instead of executing the host functions, feeds back the recorded returns in order. The guest can't tell a mic from a file, so it re-executes bit-for-bit — making a one-time, real-time, sensor-driven execution reproducible on any machine. Wasmtime enforces deterministic WASM semantics (NaN canonicalization, deterministic SIMD) so everything *between* host calls is reproducible too.

## Overhead & trace rate

**Trace rate.** Record-replay persists only values flowing
host → guest; per analysis block that's three host calls (`host-should-continue`,
`host-read-block`, `host-printf`), and the `host-read-block` return dominates. So the trace is
essentially the raw mic stream byte-for-byte (the guest decimates 48 kHz → 6 kHz *after* the
recorded return, so it doesn't shrink the trace):

| Quantity | Value | Source |
|---|---|---|
| Block payload | `YIN_RAW_LEN` × 2 B = 8192 × 2 = **16,384 B/block** | `yin.h:49` |
| Real-time block rate | `YIN_FS` / `YIN_RAW_LEN` = 48000 / 8192 = **5.86 blocks/s** | `yin.h:45,49` |
| **Steady-state trace rate** | 16,384 × 5.86 = **96,000 B/s ≈ 96 KB/s (93.75 KiB/s, 0.77 Mbit/s)** | = 48000 × 2 |

Plus <1 KB/s of postcard event framing and a **one-time ~160 KB** startup burst (the
`host-sin`/`host-cos` f64 returns from the FFT-twiddle/FIR precompute in `yin_init()` are
host → guest, so they're recorded once).

**Trace rate — measured: 96,590 B/s** on the Pi Zero 2 W, within 0.6% of the 96,000 prediction,
`overruns=0` (recording-to-file keeps up with the mic in real time). Method — two timed recordings
differenced to cancel the startup burst, plus a live `pv` over-the-wire cross-check:
**RUNBOOK Part D1**. Of that trace, only **~57 B/block (~0.35%)** is record-replay framing (postcard
event tags + the small per-block events) — it's ~99.6% raw audio payload.

**Recording / streaming CPU overhead.** Wall-clock is mic-paced (~171 ms/block), so recording
cost shows up as *lost real-time headroom* — higher CPU and ALSA **overruns** (`bridge.c`) — not
longer runtime. The metric is CPU-per-block, compared across three configs (no recording /
record→file / record→stream); build + measurement commands in **RUNBOOK Part D2**. Measured on
the Pi Zero 2 W (60 s runs, ~335 blocks each, `overruns=0` throughout):

| Config | CPU/block | Of the 170.7 ms/block real-time budget |
|---|---|---|
| 0. no recording | 17.05 ms | 10.0% of one core |
| 1. record → file | 35.41 ms | 20.7% of one core |
| 2. record → stream (FIFO→socat→TCP) | 35.29 ms | 20.7% of one core |

**Recording overhead ≈ +18.4 ms/block (~2.1×, +108% CPU)** — recording roughly doubles per-block
compute, but still uses only ~21% of the per-block budget, so ~79% headroom remains and overruns
stay at 0. Most of the +18 ms is serializing the 16 KB audio block each iteration, so host-side
decimation/compression (below) would cut it along with the trace size. **Streaming adds ≈ 0** on
the recorder (35.29 vs 35.41 ms/block, within noise): from `rpi_embed`'s view a FIFO write costs
the same as a file write, and the socat/TCP/replay work runs in separate processes — so live
streaming doesn't burden the recorder's real-time budget (overruns stayed 0). *On the wire*,
though, streaming carries the trace plus **~5.65% TCP/IP framing** (measured: 5.87 MB sent for
5.56 MB of trace, ~102 KB/s) — above the ~2.7% full-MSS textbook figure because the trace is
bursty (batched flushes → many sub-MSS segments); link- and burstiness-dependent. This comes out to 
~929B of additional data per block over the wire. 

## Known limitations / Next Steps (Suggested By Claude Code)

- **Trace size = full audio rate:** the guest pulls a whole analysis block per host call (`host-read-block`, 8192 raw samples), draining ALSA fast enough to run in real time — overruns are now the exception, not the rule (`bridge.c` still recovers from any `-EPIPE`). The remaining cost is volume: raw samples cross the boundary *before* the guest decimates, so the trace carries the full 48 kHz s16 stream — ~16 KB/block, **~96 KB/s (~0.77 Mbit/s)** (see §Overhead & trace rate). Decimating or compressing host-side before the recorded return would shrink it further.
- **High end:** after decimating 48 kHz → 6 kHz, lag resolution is coarse above ~1 kHz (e.g. 1500 Hz reads ~+1.8%). Fine across the guitar range.
- **Replay fidelity:** recorded with default settings (no validation metadata), so replay matches the event stream but not via `--validate`. Recording with validation enabled would allow the stronger `--validate` check.

## Repository layout

```
rpi/
  freq_detect_test/                native baseline (clang + ALSA)
    src/{main.c, yin.h, kiss_fft.*}
  freq_detect_wasm/
    freq_detect_embed/             WASM guest (the "tuner")
      src/{main.c, yin.h, shim.h, wasm_libc.c, kiss_fft.*}
      adc.wit                      host interface (the `rpi` world)
      wasm_component/              built tuner.wasm / *.component.wasm
    freq_detect_pulley_embed/
      wasmtime-rr-prototyping/     forked Wasmtime (adds record-replay)
        rpi_embed/                 the host binary (loads + records the component)
```

`yin.h` is the shared FFT-accelerated YIN detector (header-only, no malloc); it is byte-identical in the native and guest trees.
