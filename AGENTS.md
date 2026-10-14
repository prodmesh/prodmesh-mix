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
| `tools/scenes_e2e.py` | Scenes against a sim mixd: store, modified, recall with fade, undo, persistence |
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

## Routing and mix-minus

The console model: a **channel** (Strip) has an *input* (a device input,
a stereo pair of them, a bus, or a test source), input gain, processing
and inserts, a fader, a *main output* (buses and/or outputs) and *sends*
(to buses or outputs, each with a level and on/off). All of it is edges:
gain on the input edge is the channel's input gain; `EdgeDesc::send`
separates sends from the main output (presentation only),
`EdgeDesc::mute` silences an edge but keeps its level, and
`EdgeDesc::preFader` taps the source after processing and mute but before
its fader (the step gets a second buffer; an edge instance is fixed pre or
post like its delay, so switching crossfades). Pan on a stereo->stereo
edge is a balance. Device inputs are
fixed: `shows::normalize()` gives every device channel a mono Input node,
so every input is always recorded whether a channel uses it or not, and a
stereo channel takes two of them panned hard L/R. Output nodes exist while
something feeds them (the UI creates and prunes them).

A **mix-minus** bus (`NodeDesc::mixMinus`: `follows` + `exclude`) is a
rule, not a list: `resolveMixMinus()` (run by `Engine::setGraph` on every
edit) feeds it from every strip that reaches `follows`, post-fader, except
the excluded ones — new channels join by themselves. Those edges are
`derived`; an explicit edge from a strip to the bus overrides its automatic
one. Buses never feed a mix-minus automatically (a group may contain the
voice it must leave out).

UI: input button at the top of each channel; insert slots (click = menu,
drag = reorder); send slots (dot = on/off, "−" = left out of a mix-minus;
click = menu with pre/post, reset, remove; drag = level; last row = + Send;
"PRE" tag on pre-fader sends); pan above the fader; the "→ destination"
button; "+ Channel" and "+ Bus" tiles; click a strip's name (or
right-click it) to rename, delete, or make a mix-minus for it. Menus
toggle from the state they showed, not QAction's checked flag
(accessibility presses do not always flip it).

Plugin settings survive chain edits: a changed chain is a new instance,
so `Engine::setGraph` first copies each running plugin's current state
onto the matching insert of the new chain (`carryPluginStates`: same
plugin, in order, isolated or not). Without it, adding, removing or
reordering an insert reverted the channel's other plugins to their last
saved settings. `mixd --driver sim
--sim-inputs 8` gives silent device inputs to try it without hardware.

## Scenes

A scene is a stored copy of the graph; recalling it applies the *mix*
(`applyScene`: faders, mutes, and level / pan / on-off / pre-post of
connections present in both graphs, mix-minus exclusions) and never the
*structure* (channels, buses, patching, inserts and plugin state), so a
recall cannot remove a channel or reload a plugin. `NodeDesc::recallSafe`
nodes, and connections touching them, are skipped. Built-in EQ/dynamics
are not recalled yet: a processing change rebuilds the node instance,
plugins included — split built-in DSP from the insert chain first.

Scenes live in mixd (the session file), not the UI. `recallScene` calls
`Engine::setGraph(g, err, fadeMs)`: the plan carries a fader ramp and
glide lengths for connections that survive the edit; mutes and new or
removed connections keep their short fades, so cleanup timing is
unchanged. Later plans re-arm the same targets (a no-op) and do not cut
a glide short; an operator's fader move (newer sequence) takes over.
`undoRecall` restores the mix and the live scene from just before the
last recall. UI: select a scene, then GO (double-click = recall now);
clicking the selected scene opens its menu.

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

## Plugin hosting

`src/plugin/`: CLAP (`clap_host.cpp`) and VST3 (`vst3_host.cpp`, SDK
interface headers only) adapters behind `plugin::load()`. Lessons paid for
with segfaults in real commercial plugins — keep them:
- give **every** declared port/bus a buffer (sidechains fed silence), even
  inactive ones; FabFilter CLAPs read the sidechain unconditionally;
- VST3: pass real parameter-change containers, event lists and a process
  context, never null; plugins write meters to output parameter changes
  without checking for a null queue (FabFilter Pro-C 2 / Pro-L 2).
Never load a plugin to *scan* it in the engine: `mix-plugin scan` does it
in child processes with a timeout, and with `--cache` (as mixd runs it)
only bundles that are new or changed get loaded — loading plugins can pop
up licence dialogs (iLok) on the user's screen, so each version is loaded
at most once and quarantine is remembered. The catalog is machine-wide:
`~/Library/Application Support/ProdMesh/Mix/plugins.jsonl`.
**On the dev machine, test only with FabFilter (licensed) and MiniMeters
(free)** — the smoke test's default set.
Third-party code that allocates on the audio thread is attributed to the
plugin (`rt::ScopedThirdParty`, `Engine::pluginRtIssues()`), not counted as
an engine violation; FabFilter Pro-L 2 does it in its first blocks. `PM_PLUGIN_TRACE=1` logs host->plugin
calls; `tools/plugin_smoke.py` runs installed real plugins as inserts
(19/19 on the dev machine) — run it after touching an adapter.
**Isolation.** `InsertDesc::sandboxed` (default for UI-added plugins)
runs consecutive isolated plugins of a channel as one chain in a
`mix-pluginhost` process (`src/plugin/sandbox*.cpp`, `tools/mix_pluginhost.cpp`):
audio over shared memory + `os_sync_wait_on_address`, one block of added
latency for the chain (reported to PDC), never a wait on the audio thread.
A late or dead host -> the previous block's input passes through (same
latency, no step); `Engine::pump()` restarts it with its last reported
state, up to 3 times. The host's stdin/stdout is a line protocol
(`editor N`, `state-save` -> `state N <base64>`); plugin editors live in
the host's main thread (CLAP cocoa embedding). Plugin state is an opaque
base64 blob on `InsertDesc::state`, excluded from structural equality;
`Engine::collectPluginStates()` refreshes it before every session save.
Host render threads join their own audio work interval (deadline per
block), created *before* the host reports Ready: the first
`AudioWorkIntervalCreate` in a process costs ~100 ms, which once made a
restarted host pass audio dry. Hosts are accessory processes (no Dock
icon) that bring themselves forward when an editor opens. Misses count
only after a host's first delivered block (startup is not "late"); the
hang watchdog counts from the start. Measure with
`mix-bench --isolated N [--isolated-plugin FORMAT:PATH:ID]` and
`PM_HOST_STATS=1` (per-host wake/work timing on stderr). On the dev
machine, 16 isolated Pro-Q 3 hosts: 0 late blocks; without the work
interval, wake-ups of up to 2 ms under load (1.2% late in one run).
Not done yet: plugin parameter automation, sharing one host across
channels.

## UX rules (spec: UX principles)

- Never show stale data as if it were live: on disconnect, meters empty
  and health numbers disappear; a banner says what it means (no engine =
  no program audio).
- Faders never jump on a click; drags are relative; Option-click (or
  double-click) resets any control to its zero: 0 dB, pan centre, a
  mix-minus send back to automatic. Option- or Cmd-drag = fine.
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
