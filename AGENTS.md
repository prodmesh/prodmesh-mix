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
| `src/dsp/` | Biquads (RBJ), gate / compressor / limiter, deterministic test generator, BS.1770 loudness (`loudness.h`) |
| `src/engine/graph.h` | `GraphDesc`: control-side description (plain values) |
| `src/engine/node.*` | `NodeInstance` / `EdgeInstance`: audio-thread DSP state, live across plans |
| `src/engine/plan.h` | `RenderPlan`: compiled, immutable per-edit render order + buffers |
| `src/engine/engine.*` | Compile, publish, activate, render; the live-edit machinery |
| `src/engine/worker_pool.*` | Parallel level-by-level render, generation-tagged claims |
| `src/driver/` | `Driver` interface, simulated RT clock (with audio work interval), CoreAudio |
| `src/proto/` | Control protocol (JSON lines over a local socket), graph <-> JSON; Qt Core |
| `src/daemon/` | `mixd`, the engine daemon (Tier 0 process), and built-in show templates |
| `src/ui/` | `ProdMeshMix.app`, the mixer client: theme tokens (mirrors RTA), strip/fader/meter widgets |
| `src/record/` | Always-on recording: `RecordTap` (wait-free ring + gap log), `WavWriter` (crash-safe BWF/RF64), `Recorder` (disk-writer thread), `peaks.h` (waveform overviews) |
| `tools/t07_crash_recording.py` | T0-7 gate: kill -9 mid-take, header lag <= 1 s, restart repairs every file |
| `tools/engine_lifecycle_e2e.py` | One engine per socket; shutdown warns clients and finalises the take |
| `tools/playback_e2e.py` | Playback (virtual soundcheck) against a sim mixd: play, locate, stop at end, back to live, 0 dropouts |
| `tools/projects_e2e.py` | Projects against a sim mixd: first-run Start offered once, new/open/recent, takes per project |
| `tools/scenes_e2e.py` | Scenes against a sim mixd: store, modified, recall with fade, undo, persistence |
| `tools/t05_ui_isolation.py` | T0-5 gate: SIGKILL the UI repeatedly under load, check 0 xruns and < 2 s reconnect |
| `tools/mix_bench.cpp` | Tier 0 bench: reference graph on a driver, timing histogram, live edits |
| `tools/bench_graph.*` | The reference graph shared by bench and tests |
| `tests/` | Dependency-free harness; `mix-tests [filter]` prints `PASS` |

The engine library is headless and Qt-free on purpose. `mixd` wraps it with
Qt Core/Network; the UI is a separate process that owns no audio and
rebuilds itself from the daemon's snapshot on every (re)connect. Protocol
messages are documented at the top of `src/proto/protocol.h`.

Run it: `open build/ProdMeshMix.app` — one app: it starts its engine
from the bundle (`Contents/Helpers/ProdMesh Mix Engine.app`, see "The app
and its engine") when none is running. For development, run an engine
yourself (`./build/mixd --driver sim`, or `coreaudio` + `--audible` to hear
it) and the app connects to it instead. Without `--session`, mixd
opens the last project (or creates `~/Music/ProdMesh Mix/Untitled`).
Tools and tests must not touch the user's projects: pass `--session`, or
set `PM_MIX_HOME` to a temp dir (projects and the recent list go there). `ProdMeshMix --screenshot
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
  and RTSan/TSan should also run `mix-bench --workers 3 --edits 20` with
  **`--driver free`** as well as `sim`: back-to-back blocks make the
  control thread race the audio thread far more often (that is how the
  activate/retire use-after-free was found). CI (`.github/workflows`)
  runs all of it on every push; it cannot judge dropouts (shared VMs),
  only the soak on real hardware can.
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

## The app and its engine

One app, two processes. `ProdMeshMix.app` (the window) owns no audio; the
engine `mixd` ships inside it as a helper app,
`Contents/Helpers/ProdMesh Mix Engine.app` (bundle id
`com.prodmesh.mix.engine`, `LSUIElement`), with `mix-pluginhost` and
`mix-plugin` beside it. A helper app, not a bare binary in `MacOS/`:
otherwise macOS takes the running engine for the app and reopening the
window does nothing. The app starts the engine detached
(`--menu-bar --audible`) when nothing answers on the default socket and
restarts it after an unexpected stop (at most 3 starts a minute, then it
says so). A UI crash or "Close window only" leaves the engine running,
shown by its menu-bar icon (status, Open, Quit) — never ghost audio. A
deliberate stop (menu bar, or the app's "Quit and stop audio") sends
`shutdown` to every client, which quits instead of restarting it. One
engine per socket: a second mixd refuses to start (exit 3) rather than
take the socket. Devices / rate / buffer come from the command line, else
`audio.json` in the app data folder; no input until one is chosen (a
laptop mic into its own speakers would howl); a first run from the app
starts on the Blank template. Non-default sockets, `--no-engine` and
`--screenshot` never start an engine (tests).

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

## Projects

A project is a folder: `session.json` (graph, scenes, loudness target,
`record` setting) and `Takes/<take>/` (WAVs + markers.csv). mixd owns it:
`newProject` (template blank / inputs / livestream), `openProject`,
`projects` (recent list, templates, default location). Switching saves
the current show *before* loading the next (its scenes must not leak),
validates the new graph with `Engine::validate` (dry compile, no plugin
loads), finishes the take, then crossfades to the new mix — audio never
stops. The Start window is offered once, to the first UI of a run that
created a fresh Untitled; a reconnecting UI never sees it, and it opens
only after the first snapshot is on screen. `--session FILE` keeps the
old single-file mode (tests).

## Solo and the Monitor bus

Solo never touches Program (test: `solo_leaves_program_bit_identical`).
Every show has a Monitor bus (`NodeDesc::monitor`, added by
`shows::normalize`, recall-safe, no output until one is chosen) that
follows Program until something is soloed, then carries only the soloed
strips/buses — PFL (pre-fader, default) or AFL. Like mix-minus, it is a
rule resolved into derived edges (`resolveDerived`), so solo switches
crossfade through the normal edge ramps. `NodeDesc::solo` is monitoring
state: scenes never recall it. UI: SOLO | MUTE on strips, S on Editor
lanes (soloes the channels that source feeds), amber "SOLO · monitor
only" pill to clear, Monitor strip menu for PFL/AFL.

## Playback (virtual soundcheck)

A finished take replayed into its source nodes in place of the live
inputs, through the whole mix and out of every output — the Dante
virtual-soundcheck workflow. `record/playback.h`: one read-ahead ring per
source (`PlaybackFeed`) in a `PlaybackSet`, filled by `rec::Player`'s
reader thread; the audio thread decides once per block (`begin`) whether
it plays — only if every ring has the block, so tracks never drift; an
underrun is silence for all and counted — and fades 5 ms at play/stop.
`Engine::setPlayback(set)`: in a plan with a set, Input/Generator steps
read their feed (or are silent), never the device or the tone. A locate
builds a new set (rings are never reset under the audio thread); play /
stop asked for mid-locate are applied at the new position. mixd:
`enterPlayback` finishes the take (recording stops), `transport`
(play/stop/locate; stops at the end of the take), `exitPlayback` starts a
new take; source levels dip 5 ms around mode changes. Test:
`playback_reproduces_the_recording` (bit-exact after the fade-in). UI: an
amber banner for as long as playback lasts, "Play this take" in the
Editor, Space = play/stop, click = locate.

Recordings are big (17 tracks ≈ 11 GB/hour): e2e tools delete their temp
projects; don't leave demo daemons recording.

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

## Top bar: clock, loudness, markers

`src/ui/toolbar.h`. Program loudness per ITU-R BS.1770-4 / EBU R128
(`dsp::LoudnessMeter`: K-weighting, momentary, short-term, gated
integrated, 4x true peak), tested against the spec's coefficients and EBU
Tech 3341 cases. The audio thread only copies the program bus into a
`RecordTap` (a plan step carries up to 4 taps: recorder + meter); mixd
meters it at 10 Hz and sends `loudness`. Targets (A/85 −24/−2, R128
−23/−1, −16, −14) are per session. Markers (`addMarker`, button or M)
are written to `<take>/markers.csv` (take time, sample, wall clock).
The edit-oriented toolbar (tools, grid, nudge) waits for the timeline.

## Editor view

`src/ui/editor.h`: a project's takes on a timeline (review and navigation
for now: no playback or editing yet). It never reads audio: the recorder
writes `<track>.peaks` beside each WAV (min/max per 512 frames, int16,
append-only, flushed with the WAV headers every second) and `take.json`
(track files, names, roles, start wall clock) per take; the editor reads
those, keeps x8 reduction levels for drawing any zoom, and polls a live
take every 300 ms so it grows on screen. Waveforms are drawn in dB (−48
floor) so speech is visible; Program is the first lane; sources are raw
(pre-fader), so a muted source still shows audio. Cmd-= toggles Mixer /
Editor.

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
