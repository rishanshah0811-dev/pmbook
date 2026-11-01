# 06 · Replay: running yesterday again, exactly

**In one sentence:** Replay reads a recording and pushes every frame through the exact same code that ran live, so we get the exact same books, offline, as many times as we like.

**Why trading firms care:** You can't debug a live market: it never happens the same way twice. With a recording plus deterministic replay you can reproduce a bug from last Tuesday at 14:03:07, test a change against real data before shipping it, and benchmark on identical input. Firms keep years of recorded feeds for exactly this.

## The analogy

A football match is live once. A video recording lets you rewatch it at normal speed, fast-forward, or pause on one frame, and it's always the same match. But the video only helps if the referee (our code) makes the **same call every time** they watch it. That's **determinism**: same input, same output, every time.

## What makes ours deterministic

1. **One path.** `FrameProcessor::on_frame` is used by both `pmbook live` and `pmbook replay`. There's no "replay version" of the logic that could drift.
2. **Pure function of the frames.** Processing never looks at the clock or at connection events. Its only input is the sequence of frames.
3. **A design decision we checked against data.** Live mode used to "distrust all books on reconnect". That depends on connection events, which aren't in the recording. Looking at the recording, the server's first frame after every reconnect is an array with fresh snapshots for all 6 assets (5 connections, 5 arrays), which replace the books anyway. So the step changed nothing, and we removed it. If a stray delta ever did arrive first, the best bid/ask check would still catch it.

## Speeds

- `--speed 0`: as fast as possible (tests, benchmarks).
- `--speed 1`: real time, using the recorded gaps between frames. `--speed 10`: 10× faster.
- Gaps come from `steady_clock`, which restarts on reboot, so gaps are clamped: going backwards counts as 0, and a huge pause is capped (default 60 s).

## Worked example (real output)

```
$ make replay-sample
[replay] files=2 records=1533 feed=716.9s wall=0.003s (454363 frames/s)
[expect] MATCH: every asset's final book state hash equals data/sample/live_summary.json
```

12 minutes of live data replayed in 3 ms (release build, Apple M5, including gzip decompression). All 6 assets' final book fingerprints and every counter (36 snapshots, 2,902 deltas, 3 trades) match the live run. To prove the check has teeth, I changed one character of one expected hash, and replay reported `DIFF` and exited with an error.

## Interview questions you could be asked

1. **What can break determinism in a system like this?**
   Reading the wall clock or random numbers inside the logic, iterating a hash map whose order changes between runs and letting that order affect results, multithreaded races, uninitialised memory, and depending on events that aren't recorded (like reconnects). The fix is to make the logic a pure function of recorded inputs.
2. **Why record raw frames instead of your parsed structs?**
   Raw frames let you replay through *new* versions of the parser and book. If you record parsed output, a parser bug is baked into your data forever, and you can't test parser changes.
3. **How would you prove replay matches live?**
   Fingerprint the end state (a hash over every level) in both runs and compare. Also compare event counters, and check the comparison itself by deliberately tampering with one value.
