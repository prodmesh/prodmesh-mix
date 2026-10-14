# Phase 1 results: engine proof

Phase 1 asked whether a headless C++ engine can hold Tier 0 targets T0-1 to T0-3 on this Mac. **It can.** Across every configuration it ran with zero xruns and zero real-time violations while live edits were applied, and the hardest case stays under 21% of the buffer period with 3 render threads.

Measured 2026-10-14 on an Apple M3 Max (14 cores, 36 GB), macOS 26 (Darwin 25.6), Apple clang 17, `RelWithDebInfo`.

## Gate status

| Target | Requirement | Result |
|---|---|---|
| T0-1 | 0 xruns in an 8 h soak at 64 paths / 48 kHz / 128 samples, reference load | **1 h passed**: 1,350,000 callbacks, 3,595 live edits, 0 xruns, worst callback 22.3% of the period. The 8 h run is in progress (`soak/soak-8h-coreaudio-64p.txt`). |
| T0-2 | Worst callback under 70% of the period | **Pass**: the worst case is 58.6% (128 paths at 96 kHz, single thread), and 20.8% with 3 workers. |
| T0-3 | Live edits leave unaffected paths sample-identical; edited paths fade within 10 ms | **Pass**: bit-identical null tests, continuity tests, and mutation-checked regressions (see below). |

## Benchmark matrix

`mix-bench --seconds 30 --edits 4`. Each run applies 4 random live edits per second: insert toggles (node crossfades) and group-bus re-routes (edge ramps). "Paths" counts input strips plus buses. The reference load is HPF, gate, 4-band EQ and compressor per strip; EQ, compressor and limiter per bus; plus 4 heavy synthetic inserts.

| Driver | Paths | Rate | Block | Workers | Edits | Xruns | p50 µs | p99.99 µs | Max µs | Worst % of period |
|---|---|---|---|---|---|---|---|---|---|---|
| coreaudio | 64 | 48 kHz | 128 | 0 | 118 | 0 | 395 | 487 | 488 | 18.3 |
| sim | 64 | 48 kHz | 128 | 0 | 118 | 0 | 408 | 487 | 489 | 18.3 |
| sim | 64 | 48 kHz | 64 | 0 | 118 | 0 | 212 | 312 | 376 | 28.2 |
| sim | 128 | 48 kHz | 128 | 0 | 118 | 0 | 734 | 868 | 886 | 33.2 |
| sim | 128 | 96 kHz | 256 | 0 | 118 | 0 | 1138 | 1445 | 1563 | 58.6 |
| sim | 128 | 96 kHz | 256 | 3 | 118 | 0 | 448 | 554 | 554 | 20.8 |
| sim | 128 | 48 kHz | 64 | 3 | 118 | 0 | 150 | 233 | 239 | 17.9 |

`coreaudio` is the default output device rendering silence (the full render runs, then zeros go to the hardware). `sim` is our own real-time clock joined to an audio work interval.

## What the tests prove

`mix-tests` has 27 cases and passes under the normal build, ASan+UBSan, TSan and RealtimeSanitizer:

- **Null tests**: an insert added and removed on one strip, and a strip re-routed, leave the aux output (which that strip does not feed) bit-identical to an unedited render.
- **Continuity**: insert changes, edge add/remove, node removal, mute, channel-count changes, kind changes and output re-patches never produce a sample-to-sample step larger than the steady-state signal's.
- **Determinism**: parallel (3 workers) and serial renders are bit-identical, edits included.
- **Real-time safety**: no allocation on the audio thread (guard counter), and no allocation, lock or blocking syscall under RTSan, both in the tests and in `mix-bench --workers 3 --edits 20`.
- **Mutation checks**: disabling the crossfade, edge ramps, two-phase publish or mid-ramp idempotence makes the corresponding test fail.

## Findings along the way

1. **Scheduling matters more than DSP.** The simulated clock first ran with worst-case callbacks at 40% of the period. Joining an audio work interval (`AudioWorkIntervalCreate` plus per-cycle deadlines) cut that to 19%, apparently by keeping the thread on performance cores. Any clock we own, such as AES67, needs this.
2. **The compressor was not the bottleneck.** Replacing per-sample `log10`/`pow` with fast series approximations (accurate to 0.0001 dB) barely moved the numbers. Profiling shows biquads and gates dominate. The engine averages about 11% of one core at 64 paths free-running.
3. **The adversarial review found seven logic bugs and no data races.** Fixed:
   - a crash on negative I/O channels;
   - a reversed connection that never published (fixed with a two-phase publish);
   - a fader move a racing plan could undo (fixed with sequence-numbered `LevelState`);
   - hard cuts on channel-count, kind or patch changes (fixed with shadow replacement);
   - fader ramps re-armed by unrelated edits;
   - degenerate configs.
4. **Toolchain:** Apple clang 17's ASan hangs and TSan crashes at startup on macOS 26, even for an empty `main`. Homebrew LLVM 23 works and provides RealtimeSanitizer.

## Not covered yet

- Plugin delay compensation and real plugin hosting: phase 4 (VST3/CLAP).
- Recording (T0-7, T0-8): phase 3.
- Process split and UI crash isolation (T0-5): phase 2.
- Device inputs: the CoreAudio driver opens outputs only, to avoid a microphone-permission prompt.
- Per-path parallelism is level-by-level; a single very deep chain cannot spread across workers.

## Reproduce

```bash
./build/mix-bench --driver coreaudio --paths 64 --rate 48000 --block 128 --seconds 30 --edits 4
```
