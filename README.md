# WISE Lab Frequency Detector — Tuner Test Bed

A Raspberry Pi test bed for **record-replay of WebAssembly** on embedded hardware, exercised with a real DSP workload: an **FFT-accelerated YIN pitch detector** reading a live I2S microphone.

The same detector (`yin.h`) runs two ways:

- **Native** (`rpi/freq_detect_test/`) — plain C + ALSA, no WebAssembly. The baseline for validating the algorithm on hardware.
- **WASM + record-replay** (`rpi/freq_detect_wasm/`) — the detector compiled to a WebAssembly **component**, run on the Pi through a forked Wasmtime (`wasmtime-rr-prototyping`) that **records every host call** into a trace. That trace can then be **deterministically replayed on any machine**, with no microphone — capture-on-device, analyze-anywhere.

Detection accuracy: musical pitches 20–~900 Hz to within <0.5% (e.g. low-E 82 Hz reads 82), with a silence gate. This replaced the original 512-pt FFT peak-picker, whose 93.75 Hz bins couldn't even separate the low guitar strings.

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

### Enable I2S (one-time, on the Pi)

The mic must appear as an ALSA capture device. Add to `/boot/firmware/config.txt` (or `/boot/config.txt` on older OS):
```
dtparam=i2s=on
dtoverlay=googlevoicehat-soundcard
```
Reboot, then verify:
```bash
arecord -l                                                   # should list a capture card
arecord -D hw:0,0 -f S32_LE -c 2 -r 48000 -V mono -d 5 /dev/null   # tap the mic; the bar should move
```
If the card isn't `hw:0,0`, update that string in `freq_detect_test/src/main.c` and `rpi_embed/src/bridge.c`. If the signal is on the wrong channel, flip `ACTIVE_CHANNEL` (0 ⇄ 1).

---

## 2. Native tuner (build + run on the Pi)

Builds directly on the Pi — no cross-compile, just clang + ALSA:
```bash
sudo apt install -y clang libasound2-dev
cd rpi/freq_detect_test
clang -O2 -ffast-math -o yin_tuner src/main.c src/kiss_fft.c -lasound -lm
./yin_tuner
```
Output: `freq=<Hz> Hz (conf=<0..1>, lvl=<rms>)`, or `no signal` below the energy gate.

### Tuning to your mic (in `src/yin.h` / `src/main.c`)
- `YIN_RMS_MIN` (`yin.h`) — silence gate. Watch the printed `lvl` in silence vs. a tone and set it between them (default `0.010`).
- `s32 >> 16` (`main.c`) — input gain. Too quiet → try `>> 14`; clipping → `>> 18`.
- `YIN_CONF_MIN`, `YIN_THRESHOLD` (`yin.h`) — pitch confidence gates.

---

## 3. WASM + record-replay

The host (`rpi_embed`) is welded to the vendored Wasmtime fork (relative path dep), so it builds with the fork present. The Pi Zero is too weak to compile the fork, so build elsewhere. **On an Apple Silicon Mac this is easy: a Linux container is the same architecture as the Pi (aarch64), so we build natively — no cross-compile, no sysroot.** Match the container's distro to the Pi's so glibc lines up (this Pi runs **Ubuntu 24.04 / glibc 2.39**).

### 3a. Build environment (Apple Silicon Mac)
```bash
brew install colima docker
colima start --arch aarch64 --cpu 4 --memory 8 --disk 60
docker run --rm ubuntu:24.04 uname -m        # must print: aarch64
docker run -it --name rpibuild ubuntu:24.04 bash
```
Inside the container:
```bash
apt update && apt install -y curl git build-essential clang lld pkg-config libasound2-dev ca-certificates
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly
. "$HOME/.cargo/env"
cargo install wasm-tools
git clone https://github.com/vedlaksh/WISE-Lab-Frequency-Detector-Tuner-Test-Bed.git
cd WISE-Lab-Frequency-Detector-Tuner-Test-Bed/rpi/freq_detect_wasm/freq_detect_pulley_embed/wasmtime-rr-prototyping
rustup toolchain install nightly-2026-01-29 && rustup override set nightly-2026-01-29   # the toolchain the fork was built with
```

### 3b. Build the WASM guest → component
```bash
cd ../../freq_detect_embed
clang --target=wasm32 -O2 -ffast-math -nostdlib -Wl,--no-entry \
  -Wl,--export=run -Wl,--stack-first -Wl,--initial-memory=2097152 \
  -o wasm_component/tuner.wasm src/main.c src/kiss_fft.c src/wasm_libc.c
wasm-tools component embed adc.wit wasm_component/tuner.wasm -o wasm_component/tuner_embed.wasm
wasm-tools component new  wasm_component/tuner_embed.wasm -o wasm_component/tuner.component.wasm
```

### 3c. Build the host `rpi_embed` (native; no `RPI_SYSROOT`)
```bash
cd ../freq_detect_pulley_embed/wasmtime-rr-prototyping/rpi_embed
cargo build
strip ../target/debug/rpi_embed -o /tmp/rpi_embed     # ~202 MB debug -> ~25 MB
```
`build.rs` builds natively when `RPI_SYSROOT` is unset (this path) and cross-compiles for aarch64 only when it's set.

### 3d. Deploy + record on the Pi
From the Mac:
```bash
docker cp rpibuild:/tmp/rpi_embed ~/rpi_embed
scp ~/rpi_embed <user>@<pi>:~/
```
On the Pi:
```bash
chmod +x ~/rpi_embed && ./rpi_embed         # prints freq=... and records to ./tuner.trace
                                            # Ctrl-C to stop -> "Recording finalized"
```
Override the trace path with `RPI_TRACE=/path ./rpi_embed`.

### 3e. Replay (anywhere — no mic needed)
Build the CLI **with the `rr` feature** (the `replay` subcommand is feature-gated):
```bash
cd <fork root> ; cargo build --bin wasmtime --features rr
```
Bring the trace to the build machine (`scp` from Pi, `docker cp` into the container), then:
```bash
./target/debug/wasmtime replay --trace tuner.trace <path>/tuner.component.wasm
```
**Success = clean exit, no `"Unexpected event"`.** Host side effects (the `freq=` prints) do *not* reappear — replay restores recorded host-return values without re-running the host. Add `WASMTIME_LOG=debug … | grep "replay event"` to watch the recorded events being consumed.

---

## How record-replay works here

Every value crossing from the outside world into the WASM enters through one host-call boundary (`host_read_sample`, `host_should_continue`, …). Recording writes each host-call **return value** into the trace; replay re-runs the same WASM but, instead of executing the host functions, feeds back the recorded returns in order. The guest can't tell a mic from a file, so it re-executes bit-for-bit — making a one-time, real-time, sensor-driven execution reproducible on any machine. Wasmtime enforces deterministic WASM semantics (NaN canonicalization, deterministic SIMD) so everything *between* host calls is reproducible too.

## Known limitations / next steps

- **Throughput / `overrun`s:** the guest reads one sample per host call (8192/block), each recorded. That's too slow to drain ALSA in real time, so each block overruns (handled/reset by `bridge.c`) — detection still works and the trace is still valid, but it's laggy and the trace is large. The fix is a **block-read host import** (one call returns a whole block), which would make it real-time and shrink the trace.
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
