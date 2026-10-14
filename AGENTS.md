# ProdMesh Mix — contributor / agent guide

A live-first DAW: a broadcast console that records. Product and architecture
spec: <https://claude.ai/code/artifact/29d16b59-0e7e-4695-bd7d-aaf298e414f2>.
Read its "Tier 0" section before touching anything under `src/`.

**Tier 0 = program audio never drops.** Every change to the engine is judged
first by whether it can cause a dropout, a click on an unaffected path, or
an allocation/lock on the audio thread.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | Wait-free SPSC queue, real-time guard, RT thread/workgroup helpers, parameter ramp |
| `src/dsp/` | Biquads (RBJ), gate / compressor / limiter, deterministic test generator |
| `src/engine/graph.h` | `GraphDesc`: control-side description (plain values) |
| `src/engine/node.*` | `NodeInstance` / `EdgeInstance`: audio-thread DSP state, live across plans |
| `src/engine/plan.h` | `RenderPlan`: compiled, immutable per-edit render order + buffers |
| `src/engine/engine.*` | Compile, publish, activate, render; the live-edit machinery |
| `src/engine/worker_pool.*` | Parallel level-by-level render, generation-tagged claims |
| `src/driver/` | `Driver` interface, simulated RT clock (with audio work interval), CoreAudio |
| `src/proto/` | Control protocol (JSON lines over a local socket), graph <-> JSON; Qt Core |
| `src/daemon/` | `mixd`, the engine daemon (Tier 0 process), and built-in show templates |
| `src/ui/` | `ProdMeshMix.app`, the mixer client: theme tokens (mirrors RTA), strip/fader/meter widgets |
| `src/record/` | Always-on recording: `RecordTap` (wait-free ring + gap log), `WavWriter` (crash-safe BWF/RF64), `Recorder` (disk-writer thread) |
| `tools/t07_crash_recording.py` | T0-7 gate: kill -9 mid-take, header lag <= 1 s, restart repairs every file |
| `tools/t05_ui_isolation.py` | T0-5 gate: SIGKILL the UI repeatedly under load, check 0 xruns and < 2 s reconnect |
| `tools/mix_bench.cpp` | Tier 0 bench: reference graph on a driver, timing histogram, live edits |
| `tools/bench_graph.*` | The reference graph shared by bench and tests |
| `tests/` | Dependency-free harness; `mix-tests [filter]` prints `PASS` |

The engine library is headless and Qt-free on purpose. `mixd` wraps it with
Qt Core/Network; the UI is a separate process that owns no audio and
rebuilds itself from the daemon's snapshot on every (re)connect. Protocol
messages are documented at the top of `src/proto/protocol.h`.

Run it: `./build/mixd --driver sim` (or `coreaudio`; add `--audible` to
hear it), then `open build/ProdMeshMix.app`. `ProdMeshMix --screenshot
out.png --after 2000` renders the window to a PNG and quits — use it to
check UI changes visually.

## How a live edit works

1. Control thread calls `Engine::setGraph(desc)`. It is validated by a dry
   compile; edits coalesce while a transition is in flight.
2. `pump()` compiles against the published graph: unchanged nodes **reuse
   their instance** (state intact); a changed processing chain gets a new
   instance with the old one attached as `fadeOut` (crossfade); a changed
   kind / channel count / device patch is a **replacement** — the old node
   stays as a dying "shadow" step with its old edges while the new one's
   edges ramp in; removed nodes and edges stay as `dying` for one
   transition. If dying edges would form a cycle with new ones (reversing a
   connection), the edit publishes in two steps: removals, then additions.
3. The plan goes through `pending_` (SPSC). The audio thread activates it at
   the next block boundary, applies edge/level targets as ramps, and pushes
   the old plan to `retired_` — never frees it.
4. After `transitionMs`, the control thread publishes a cleanup plan without
   the dying/fading parts (bit-identical output) and frees retired plans.

Fader and mute live in a per-node `LevelState`, not the instance, so a
crossfade shares one level. Every level change — from a plan or from
`setLevel` — carries a sequence number and the newest wins, whatever order
the audio thread sees them in; a `setLevel` that races an in-flight plan
triggers a re-send of all levels once that plan is active.

Rules that keep unaffected paths bit-identical: contributions are summed in
sorted source-id order; `LinearRamp::setTarget` with the current target is a
no-op; the control thread only writes to instances the audio thread has not
seen yet — everything else changes through plan activation or a queue.

## Build & test

```
cmake -S . -B build -G Ninja && cmake --build build
./build/mix-tests                      # must print PASS
./build/mix-bench --driver sim --seconds 20 --edits 4
```

- Sanitizers need **Homebrew LLVM** (`brew install llvm`): Apple clang 17's
  ASan hangs and TSan crashes at startup on macOS 26 (even an empty
  `main`), and it has no RealtimeSanitizer. One build dir each:
  ```
  cmake -S . -B build-rtsan -G Ninja -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DPM_RTSAN=ON
  ```
  with `-DPM_SANITIZE=ON` (ASan+UBSan), `-DPM_TSAN=ON`, `-DPM_RTSAN=ON`;
  `-DPM_UBSAN=ON` also works with Apple clang. All must pass `mix-tests`,
  and RTSan/TSan should also run `mix-bench --workers 3 --edits 20`.
- RTSan checks everything reachable from functions marked `PM_NONBLOCKING`
  (`Engine::process`, `WorkerPool::work`): mark new audio-thread entry
  points the same way.
- `mix-bench` exits non-zero on any xrun or real-time violation.
  `--driver free` measures raw throughput (no clock, no RT policy — its tail
  latencies are meaningless); `sim` is a real-time clock with an audio work
  interval; `coreaudio` uses the default output device and **renders
  silence unless `--audible`**.
- After any engine change, run the null/continuity tests *and* a mutation
  sanity check if you touched the fade logic: disabling the crossfade must
  make `insert_change_crossfades` fail.

## Gotchas

- **Never allocate, lock, log or free on the audio thread.** `rt::ScopedRealtime`
  marks a thread real-time; operator new/delete there counts a violation
  (tests and bench fail on any). It cannot see raw `malloc` or mutexes —
  the RTSan build covers those.
- The compiler may elide a `new`/`delete` pair; tests that expect an
  allocation must make the pointer escape (`int* volatile`).
- A thread that asks for a time-constraint policy and then never sleeps is
  throttled by macOS — only promote threads that block every period.
- Render threads must join the device's audio workgroup (or our own audio
  work interval) or Apple Silicon may run them on E-cores; measured effect
  on this machine: worst callback 40% -> 19% of the period.
- clangd may show false errors until `build/compile_commands.json` exists
  (it is symlinked to the repo root by the first configure).

## Recording

`mixd --record DIR` records every source (raw) and the program bus from
engine start, one take folder per run, files named after their strips.
Taps hang off plan steps like meters; the audio thread only copies into a
ring. Rules: the disk writer may block, the audio thread may not; a full
ring drops the block and queues a gap, which the writer fills with
silence so all files of a take stay sample-aligned. T0-8 check:
`mix-bench --record DIR --stall-ms 10000` (must drop 0 frames with the
default 30 s rings; a 5 s ring must report drops but keep 0 xruns).
Power-loss durability (F_FULLFSYNC) is not done yet — fsync covers process
and OS crashes only.

## UX rules (spec: UX principles)

- Never show stale data as if it were live: on disconnect, meters empty
  and health numbers disappear; a banner says what it means (no engine =
  no program audio).
- Faders never jump on a click; drags are relative; double-click = 0 dB.
- Colour only for meaning: accent = signal/OK, amber = hot, red =
  clip/mute/fault. Theme tokens live in `src/ui/theme.h` (from RTA).
- Look at a `--screenshot` of every UI change before calling it done.

## Conventions

- C++20, 4-space indent, ~80-column lines, sparse comments that explain
  *why* (constraints, units, standards), matching ProdMesh RTA/Caption.
- No GPL code or close paraphrase. Reference material policy (clean room
  for Audacity/Ardour, Pro Tools binaries off-limits pending legal review):
  spec section "IP and reference-material policy".
- Every Tier 0 SLO gets an automated check before the feature it guards.
