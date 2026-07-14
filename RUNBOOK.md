# Runbook — List of Relevant Commands

The single command reference for the WASM record-replay tuner: enable the mic, build the
binaries, deploy, record, replay, live-stream, and measure trace rate + overhead.

## Placeholders (set once, reused below)

| Placeholder | Meaning |
|---|---|
| `<target-ip>` | the **Mac's LAN IP** — `ipconfig getifaddr en0` on the Mac (try `en1` if blank / on Ethernet). The Pi dials the Mac, which forwards `-p 9000` into the container. |
| `<pi>` | your Pi SSH login, e.g. `evaluator@crimp-pi0.local` |

Conventions used throughout: port `9000`, FIFO `/tmp/tuner.fifo`, container name `rpibuild-stream`.

---

# Part A — One-time setup & builds

## A1. Pi — enable the I2S mic

Wiring is in README §Hardware. Add to `/boot/firmware/config.txt` (or `/boot/config.txt` on older OS), then reboot:
```
dtparam=i2s=on
dtoverlay=googlevoicehat-soundcard
```
```bash
# PI — verify the mic shows up and captures
arecord -l                                                        # should list a capture card
arecord -D hw:0,0 -f S32_LE -c 2 -r 48000 -V mono -d 5 /dev/null  # bar should move
```
If the card isn't `hw:0,0`, update that string in `freq_detect_test/src/main.c` and `rpi_embed/src/bridge.c`; if the signal's on the wrong channel, flip `ACTIVE_CHANNEL` (0 ⇄ 1).

## A2. Pi — native baseline tuner (optional, no WASM)

Validates the algorithm on hardware. Builds directly on the Pi:
```bash
# PI
sudo apt install -y clang libasound2-dev git
git clone https://github.com/vedlaksh/WISE-Lab-Frequency-Detector-Tuner-Test-Bed.git
cd WISE-Lab-Frequency-Detector-Tuner-Test-Bed/rpi/freq_detect_test
clang -O2 -ffast-math -o yin_tuner src/main.c src/kiss_fft.c -lasound -lm
./yin_tuner                                # freq=… Hz (conf, lvl) | no signal
```
Tuning knobs (`src/yin.h`, `src/main.c`): `YIN_RMS_MIN` (silence gate), `s32 >> 16` (input gain), `YIN_CONF_MIN`/`YIN_THRESHOLD` (confidence). See README §2.

## A3. Mac — Colima + build container

The Pi Zero is too weak to compile the Wasmtime fork, so build in an aarch64 Linux container (same arch as the Pi — native, no cross-compile). Match the Pi's distro (Ubuntu 24.04 / glibc 2.39).
```bash
# MAC
brew install colima docker
colima start --arch aarch64 --cpu 4 --memory 8 --disk 60
docker run --rm ubuntu:24.04 uname -m                    # must print: aarch64
# Create the container WITH the streaming port published, so no re-run is needed later:
docker run -dit --name rpibuild-stream -p 9000:9000 ubuntu:24.04 bash
docker exec -it rpibuild-stream bash
```
> Already have a portless `rpibuild` with the toolchain? Snapshot it instead of rebuilding:
> `docker commit rpibuild rpibuild:stream && docker run -dit --name rpibuild-stream -p 9000:9000 rpibuild:stream bash`

```bash
# CONTAINER — toolchain + clone
apt update && apt install -y curl git build-essential clang lld pkg-config libasound2-dev ca-certificates socat
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly
. "$HOME/.cargo/env"
cargo install wasm-tools
git clone https://github.com/vedlaksh/WISE-Lab-Frequency-Detector-Tuner-Test-Bed.git
REPO=/WISE-Lab-Frequency-Detector-Tuner-Test-Bed
FORK="$REPO/rpi/freq_detect_wasm/freq_detect_pulley_embed/wasmtime-rr-prototyping"
COMPONENT="$REPO/rpi/freq_detect_wasm/freq_detect_embed/wasm_component/tuner.component.wasm"
WASMTIME="$FORK/target/debug/wasmtime"
cd "$FORK" && rustup toolchain install nightly-2026-01-29 && rustup override set nightly-2026-01-29
```

## A4. Container — build the WASM guest → component
```bash
# CONTAINER
cd "$REPO/rpi/freq_detect_wasm/freq_detect_embed"
clang --target=wasm32 -O2 -ffast-math -nostdlib -Wl,--no-entry \
  -Wl,--export=run -Wl,--stack-first -Wl,--initial-memory=2097152 \
  -o wasm_component/tuner.wasm src/main.c src/kiss_fft.c src/wasm_libc.c
wasm-tools component embed adc.wit wasm_component/tuner.wasm -o wasm_component/tuner_embed.wasm
wasm-tools component new  wasm_component/tuner_embed.wasm -o wasm_component/tuner.component.wasm
test -f "$COMPONENT" && echo "component OK"
```

## A5. Container — build the host `rpi_embed` + the replay CLI
```bash
# CONTAINER
cd "$FORK/rpi_embed" && cargo build                      # native build (RPI_SYSROOT unset)
strip ../target/debug/rpi_embed -o /tmp/rpi_embed        # ~202 MB debug -> ~25 MB
# Replay CLI: the `replay` subcommand is gated behind the rr feature
cd "$FORK" && cargo build --bin wasmtime --features rr
test -x "$WASMTIME" && echo "wasmtime OK"
```

## A6. Deploy `rpi_embed` to the Pi
```bash
# MAC
docker cp rpibuild-stream:/tmp/rpi_embed ~/rpi_embed
scp ~/rpi_embed <pi>:~/
ssh <pi> 'chmod +x ~/rpi_embed && ~/rpi_embed --help >/dev/null 2>&1; echo deployed'
```
```bash
# PI — install measurement/streaming deps once
sudo apt-get install -y socat pv time
```

## A7. Re-entering the container later

`docker run` *creates*; it collides on `--name` if re-run. The container persists on disk after you exit (no `--rm`), so use `start`/`exec`:
```bash
# MAC
docker start rpibuild-stream                 # restart the stopped container
docker exec -it rpibuild-stream bash         # open a shell in it
```
The fresh shell loses the A3 environment, so re-export the paths (and re-source cargo) before any build/replay step below:
```bash
# CONTAINER — restore the A3 shell variables (order matters: REPO first)
. "$HOME/.cargo/env"
REPO=/WISE-Lab-Frequency-Detector-Tuner-Test-Bed
FORK="$REPO/rpi/freq_detect_wasm/freq_detect_pulley_embed/wasmtime-rr-prototyping"
COMPONENT="$REPO/rpi/freq_detect_wasm/freq_detect_embed/wasm_component/tuner.component.wasm"
WASMTIME="$FORK/target/debug/wasmtime"
```
Detach without stopping it: **Ctrl-P Ctrl-Q**. A stopped container costs only disk; the real cost is Colima's VM RAM/CPU — reclaim with `colima stop`, resume with `colima start` (flags remembered).

---

# Part B — Record & replay (file)

## B1. Record on the Pi
```bash
# PI — records to ./tuner.trace; Ctrl-C -> "Recording finalized"
./rpi_embed                                  # prints freq=…; override path with RPI_TRACE=/path
ls -l tuner.trace
```

## B2. Replay anywhere (no mic)

Bring the trace to the build machine (`scp` from Pi, `docker cp` into the container), then:
```bash
# CONTAINER
"$WASMTIME" replay --trace tuner.trace "$COMPONENT"
```
**Success = clean exit, no `Unexpected event`.** Host side effects (the `freq=` prints) do *not* reappear — replay feeds back recorded host-return values without re-running the host. Watch events flow: `WASMTIME_LOG=debug "$WASMTIME" replay … 2>&1 | grep "replay event"`.

---

# Part C — Live streaming (record on Pi → replay in container, no file copy)

Point both ends at a FIFO and bridge them with `socat` over TCP. No code changes — the recorder
writes whatever `RPI_TRACE` names, `replay --trace` reads whatever path it's given; the in-band
`Eof` marker (written on Ctrl-C) ends replay cleanly.

```
Pi:  rpi_embed ─write→ /tmp/tuner.fifo ─socat→ TCP:9000 ─socat→ /tmp/tuner.fifo ─read→ wasmtime replay  :container
```

## C1. Make the port reachable + create FIFOs
```bash
# MAC — get the dial-in IP (see the placeholder table for why it's the Mac IP, not the VM)
ipconfig getifaddr en0                       # <target-ip>  (try en1 if blank / on Ethernet)
```
```bash
# CONTAINER and PI — one FIFO on each (reusable; never rm into a regular file)
test -p /tmp/tuner.fifo || mkfifo /tmp/tuner.fifo ; ls -l /tmp/tuner.fifo   # leading "p" = FIFO
```

## C2. Smoke-test the transport (before binaries)
```bash
# CONTAINER (one shell)
socat -u TCP-LISTEN:9000,reuseaddr OPEN:/tmp/tuner.fifo,wronly &
cat /tmp/tuner.fifo
```
```bash
# PI
nc -vz <target-ip> 9000                                       # must succeed before continuing
socat -u OPEN:/tmp/tuner.fifo,rdonly TCP:<target-ip>:9000,retry=30,interval=1 &
echo "hello-stream" > /tmp/tuner.fifo                         # container's cat prints hello-stream
```
Clean up after (`kill %1`/`pkill socat` each side). Do **not** proceed until `hello-stream` arrives.

## C3. Live run (4 shells, consumer first)
```bash
# CONTAINER shell 1 — replay (blocks until data flows)
"$WASMTIME" replay --trace /tmp/tuner.fifo "$COMPONENT"
```
```bash
# CONTAINER shell 2 — bridge listener
socat -u TCP-LISTEN:9000,reuseaddr OPEN:/tmp/tuner.fifo,wronly
```
```bash
# PI shell 1 — bridge dialer (+ live trace-rate meter via pv)
pv -btra /tmp/tuner.fifo | socat -u - TCP:172.20.10.2:9000,retry=30,interval=1
```
```bash
# PI shell 2 — record into the FIFO
RPI_TRACE=/tmp/tuner.fifo ./rpi_embed
```
Ctrl-C the Pi recorder (shell 2) when done → replay prints `All replay events were successfully processed.` (with no `Unexpected event`/`error`/`panic`/`trap` above it — the definitive success signal). Use `OPEN:…,rdonly`/`,wronly`, **not** `PIPE:` (which holds the FIFO `O_RDWR` so replay never sees EOF). Add `,fork` to the listener for repeated runs without restarting it.

## C4. Prove it's *live* (not buffered-then-replayed)

The bridge is a FIFO with a small fixed buffer (~64 KB pipe + socat + socket ≈ a few hundred KB,
can't grow). A multi-second trace is far larger, so the Pi can only stream `freq=` continuously
if replay is draining concurrently. The undeniable check — the **stall test** (chain running,
replay logging via `WASMTIME_LOG=debug … | grep "replay event"`):
1. In the Pi recorder shell, **Ctrl-Z** (suspend `rpi_embed`) → within a fraction of a second replay's events **stop advancing** (small buffer drained, blocked on the live pipe).
2. `fg` to resume → replay **immediately** resumes.

Lockstep freeze/resume = no buffered file behind replay, only the live pipe.

---

# Part D — Measure

## D1. Trace rate (data/sec)

Two timed recordings; the **difference cancels** the fixed ~160 KB startup burst. Use
`timeout --signal=INT` (clean stop at the deadline) — **not** a background `&` + `kill`, which
races: if the recorder exits early the window is wrong and the rate is bogus.
```bash
# PI
pkill -f rpi_embed 2>/dev/null               # release the mic from any straggler
for T in 20 60; do
  timeout --signal=INT --preserve-status "${T}s" ./rpi_embed >/dev/null 2>"rec_$T.err"
  sz=$(stat -c%s tuner.trace); eval "B$T=$sz"            # Pi/Linux; macOS: stat -f%z
  echo "T=${T}s -> ${sz} bytes  overruns=$(grep -c overrun rec_$T.err)"
done
echo "steady-state trace rate = $(( (B60 - B20) / 40 )) B/s"   # expect ~96000
```
Sanity-check before trusting: **B20 ≈ 1.7 MB, B60 ≈ 5.6 MB**. A few-KB B20 means that run never
got past ALSA init (mic busy). The live `pv` meter (Part C3) is the over-the-wire cross-check;
its instantaneous rate jitters (bursty flushing) — read its **average** column (~94 KiB/s).
Measured result + derivation: README §Overhead & trace rate.

**Format overhead** — how much of the trace is RR framing vs. the 16,384 B/block audio payload.
Same two-run difference, but capture stdout to count blocks:
```bash
# PI
pkill -f rpi_embed 2>/dev/null
for T in 20 60; do
  timeout --signal=INT --preserve-status "${T}s" ./rpi_embed >"out_$T.log" 2>/dev/null
  eval "B$T=$(stat -c%s tuner.trace)"; eval "N$T=$(grep -cE 'freq=|no signal' out_$T.log)"
done
awk -v db=$((B60-B20)) -v dn=$((N60-N20)) 'BEGIN{b=db/dn; o=b-16384;
  printf "trace=%.1f B/block  payload=16384  framing=%.1f B/block (%.2f%%)\n",b,o,o/b*100}'
```
Measured: **~57 B/block (~0.35%)** — the trace is ~99.6% raw audio.

## D2. Recording & streaming overhead (CPU/block)

Wall-clock is mic-paced (~171 ms/block), so overhead shows up as lost real-time headroom — higher
CPU and ALSA **overruns** — not longer runtime. Compare CPU/block across three configs.

**D2a. Build an rr-disabled baseline (container), deploy to Pi:**
```bash
# CONTAINER — disable rr + comment the record sink, build, restore source
cd "$FORK/rpi_embed/src" && cp main.rs main.rs.orig
sed -i \
  -e 's/config\.rr(RRConfig::Recording);/config.rr(RRConfig::None);/' \
  -e 's@^\( *\)\(let trace_file = std::fs::File::create.*\)@\1// \2@' \
  -e 's@^\( *\)\(let trace_writer = std::io::BufWriter::new.*\)@\1// \2@' \
  -e 's@^\( *\)\(store\.record(trace_writer, rs)\.unwrap();\)@\1// \2@' \
  main.rs
grep -nE 'config\.rr\(|trace_file|trace_writer|store\.record' main.rs   # eyeball: None + 3 commented
cd "$FORK/rpi_embed" && cargo build && strip ../target/debug/rpi_embed -o /tmp/rpi_embed_norr
mv src/main.rs.orig src/main.rs
```
```bash
# MAC
docker cp rpibuild-stream:/tmp/rpi_embed_norr ~/rpi_embed_norr
scp ~/rpi_embed_norr <pi>:~/ ; ssh <pi> 'chmod +x ~/rpi_embed_norr'
```

**D2b. Measure (Pi).** Paste the helper, run three configs:
```bash
# PI — runs a binary 60 s, clean-stops it, prints CPU/block + overruns
measure() {  # $1=label  $2=binary  $3=optional env (e.g. RPI_TRACE=/tmp/tuner.fifo)
  pkill -f rpi_embed 2>/dev/null
  env ${3:-} /usr/bin/time -v -o "time_$1.log" "$2" >"out_$1.log" 2>"over_$1.log" &
  local t=$!; sleep 60; kill -INT "$(pgrep -P $t)" 2>/dev/null; wait $t 2>/dev/null
  local cpu=$(awk -F': ' '/User time|System time/{c+=$2} END{print c}' "time_$1.log")
  local blk=$(grep -cE 'freq=|no signal' "out_$1.log")
  local ovr=$(grep -c overrun "over_$1.log")
  awk -v l="$1" -v c="$cpu" -v b="$blk" -v o="$ovr" \
    'BEGIN{printf "%-7s cpu=%.2fs blocks=%d overruns=%d cpu/block=%.3f ms\n",l,c,b,o,(b?c/b*1000:0)}'
}

measure norr   ~/rpi_embed_norr                         # 0. baseline (no recording)
measure file   ~/rpi_embed                              # 1. record -> ./tuner.trace
# Config 2 needs the Part C3 stream up (container replay + listener, Pi dialer) first:
measure stream ~/rpi_embed   RPI_TRACE=/tmp/tuner.fifo  # 2. record -> FIFO -> container
```
Recording overhead = (1) − (0); streaming overhead = (2) − (1); new overruns in (2) mean TCP
backpressure stalling the Pi's `write()`. Measured numbers + interpretation: README §Overhead & trace rate.

## D3. Streaming wire overhead (optional, network-dependent)

Bytes actually sent over TCP vs. the trace payload. Bring up the Part C3 container side first
(replay + listener), then on the Pi:
```bash
# PI shell 1 — instrumented dialer (exits on its own when the recorder finishes)
IFACE=$(ip route | awk '/default/{print $5; exit}')
A=$(cat /sys/class/net/$IFACE/statistics/tx_bytes)
cat /tmp/tuner.fifo | tee /tmp/payload.bin | socat -u - TCP:<target-ip>:9000,retry=30,interval=1
B=$(cat /sys/class/net/$IFACE/statistics/tx_bytes)
awk -v w=$((B-A)) -v p=$(stat -c%s /tmp/payload.bin) 'BEGIN{
  printf "payload=%d B  wire=%d B  TCP/IP overhead=%d B (%.2f%%)\n",p,w,w-p,(w-p)/p*100}'
```
```bash
# PI shell 2 — recorder (60 s)
RPI_TRACE=/tmp/tuner.fifo timeout --signal=INT 60s ./rpi_embed >/dev/null 2>&1
```
Sanity: `payload` must be ~5.5 MB (KB = the container side isn't reading — restart replay +
listener). Measured **~5.65%** on a WiFi hotspot (bursty stream → sub-MSS segments, above the
~2.7% full-MSS figure); the NIC counter also catches SSH/background TX, so run it quiet.

---

# Part E — Profile the DSP under Wizard (Mac, no mic)

A second analysis path, independent of the Wasmtime RR flow above: run the **unmodified** core
`tuner.wasm` under the **Wizard** engine and emit a Graphviz self-time profile of the DSP call tree.
Runs **natively on the Mac** (not the container/Pi). Wizard supplies the `rpi.*` imports itself — a
project-authored Virgil host module (`wizard-engine/src/modules/wizeng/RpiModule.v3`) that replays audio
from `capture.pcm` on disk — so there is **no mic, no `-DRPI_LOCAL`, no `rpi_stub.c`, no recompiled
replay module**. `host-read-block` hand-rolls the component-model canonical-ABI lowering (call the
guest's exported `cabi_realloc`, copy the block into guest memory, write the `{ptr,len}` descriptor).

Prereqs (once). The engine + `capture.pcm` live **outside the repo** (Virgil's build scripts break on
spaces in the repo path), at `~/wizard-toolchain/{virgil, wizard-engine}`:
```bash
# MAC
brew install llvm lld openjdk graphviz          # graphviz/dot may already be present
WZ=~/wizard-toolchain                            # Wizard engine home (outside the repo)
# Build wizeng once, and after any RpiModule.v3 edit (wizeng.jvm is an arch-independent jar):
export PATH="$WZ/virgil/bin:/opt/homebrew/opt/openjdk/bin:$PATH"
cd "$WZ/wizard-engine" && ./build.sh wizeng jvm
```

## E1. Capture the audio on the Pi, move it to the Mac

The recorder mirrors each mic block to a raw little-endian-int16 file when `CAPTURE_PCM` is set (the
`bridge.c` tap) — one run writes both `tuner.trace` and `capture.pcm`. Needs an `rpi_embed` built from
the current tree (rebuild/redeploy per A5–A6 if your deployed binary predates the tap):
```bash
# PI — Ctrl-C to finalize, same as B1; produces tuner.trace + capture.pcm side by side
CAPTURE_PCM=~/capture.pcm ./rpi_embed
```
```bash
# MAC — move capture.pcm to where the Wizard host reads it by default (the run cwd, $WZ)
scp <pi>:~/capture.pcm "$WZ/capture.pcm"
ls -l "$WZ/capture.pcm"                           # size = N * 16384 B (one 8192-sample block = 16384 B)
```

## E2. Build the core `tuner.wasm` with its `rpi` imports intact (Mac)

**Not** the container component build (A4): Wizard runs a **core** module, and the committed
`wasm_component/*.wasm` are stale (old `host-read-sample`, no `cabi_realloc`). Rebuild from this Mac
checkout's sources — no `-DRPI_LOCAL` (imports survive), `-fno-inline` (keeps `yin_decimate` a distinct
node), and export `cabi_realloc` (so the host can allocate guest memory):
```bash
# MAC
export PATH="/opt/homebrew/opt/lld/bin:$PATH"
MREPO="$HOME/path/to/WISE-Lab-Frequency-Detector-Tuner-Test-Bed"   # this Mac checkout (quote it — real path has spaces)
G="$MREPO/rpi/freq_detect_wasm/freq_detect_embed/src"
/opt/homebrew/opt/llvm/bin/clang --target=wasm32 -O2 -ffast-math -nostdlib \
  -fno-inline -fno-inline-functions -I "$G" \
  -Wl,--no-entry -Wl,--export=run -Wl,--export=cabi_realloc \
  -Wl,--stack-first -Wl,--initial-memory=8388608 \
  -o /tmp/tuner_core.wasm "$G/main.c" "$G/kiss_fft.c" "$G/wasm_libc.c"
strings /tmp/tuner_core.wasm | grep -E 'host-read-block|cabi_realloc'   # both must appear
```

## E3. Run the profile monitor + render
```bash
# MAC — wizeng.jvm needs java; capture.pcm is read from cwd (or pass an absolute path as a trailing arg)
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd "$WZ"                                          # so ./capture.pcm resolves
wizard-engine/bin/wizeng.jvm --monitors='profile{dot}' --colors=false /tmp/tuner_core.wasm > tuner.profile.dot
dot -Tsvg tuner.profile.dot -o tuner.profile.svg
# path-override form (run from any cwd):
# wizard-engine/bin/wizeng.jvm --monitors='profile{dot}' /tmp/tuner_core.wasm /abs/path/capture.pcm > tuner.profile.dot
```
Repo convenience wrapper (override its engine path): `WIZENG="$WZ/wizard-engine/bin/wizeng.jvm"
"$MREPO/wizard/profile_dot.sh" /tmp/tuner_core.wasm`.

**Success = clean exit, no trap for unresolved `rpi.*`, and `yin_decimate` dominates (~89% self-time)**
with the FFT `kf_bfly4` butterflies ~1–2% each. `host_sin`/`host_cos` run in the Wizard host, so they do
**not** appear in the graph (that's the intended difference from the old embedded build). A missing/empty
`capture.pcm` → `host-should-continue` returns 0 → zero iterations → a degenerate but trap-free profile,
which is itself proof the audio is read at runtime.

---

# Part F — Teardown
```bash
# PI:  Ctrl-C / pkill any leftover socat, rpi_embed, pv
# MAC:
docker stop rpibuild-stream      # frees the container (disk persists)
colima stop                      # frees the VM's RAM/CPU
```

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| A command just hangs | A FIFO is waiting for its peer. Both the `socat` **and** the binary on that side must be running; the chain only flows once every link is up. |
| Replay returns instantly / 0 events | The four processes weren't all alive at once (e.g. replay + listener run sequentially in one shell). Run each in its own shell, consumer first (Part C3). |
| `nc -vz` fails / times out on the Pi | Wrong `<target-ip>` (used the host-only Colima VM address) or the container listener isn't up. Use `ipconfig getifaddr en0`; confirm `socat …TCP-LISTEN:9000…` is running. Still failing on the Mac IP → check the Mac firewall + use a live interface (`ifconfig \| grep "inet "`). |
| Replay never ends after Pi Ctrl-C | You used `PIPE:` instead of `OPEN:…,rdonly/wronly`. `PIPE:` opens the FIFO `O_RDWR`, so the read side never sees EOF. |
| "Streaming" produces nothing / replay empty | The FIFO path was a regular file when a binary opened it. `rm -f /tmp/tuner.fifo && mkfifo /tmp/tuner.fifo` (the `ls -l` "p" check catches this). |
| `wasmtime: replay subcommand not found` | CLI built without `--features rr`. Rebuild (A5). |
| Replay errors mid-stream, no clean end | Recorder died without writing `Eof`. Re-run; only a clean Ctrl-C finalizes the trace. |
| Trace-rate run gives a wild number | The recorder exited before the timer (`kill: No such process`). Use the `timeout` form in D1, and check the mic isn't held by a stray `rpi_embed`. |
| `measure` shows tiny `blocks` | Mic busy (stray `rpi_embed` on `hw:0,0`) → ALSA init failed fast. `pkill -f rpi_embed` and redo. |
| Wizard: `Unable to locate a Java Runtime` | `wizeng.jvm` needs `java`. `export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"` (Part E). |
| Wizard: unresolved `rpi.*` import / trap at load | You ran a stale `wasm_component/*.wasm` (old `host-read-sample`, no `cabi_realloc`). Rebuild the core module from `src/` (E2). |
| Wizard: "no entrypoint" or a degenerate/empty profile | `capture.pcm` not found in cwd (run from `$WZ` or pass an absolute path as a trailing arg), or the module was built without `-Wl,--export=cabi_realloc` / `-Wl,--export=run`. |
| `clang: … wasm-ld … not found` | `wasm-ld` ships in the separate `lld` formula. `brew install lld && export PATH="/opt/homebrew/opt/lld/bin:$PATH"`. |

*Why no code change is needed for streaming: record path via `RPI_TRACE` (`rpi_embed/src/main.rs:63-64`), read path via `--trace` (`src/commands/replay.rs:107`) — both are plain sequential stream I/O, and a FIFO is a valid path for each.*
