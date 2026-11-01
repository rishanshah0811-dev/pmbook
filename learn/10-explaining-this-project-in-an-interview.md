# 10 · Explaining this project in an interview

**In one sentence:** Have a 30-second version, a 2-minute version, and three deep-dive stories ready, and be the first to say what you *didn't* measure.

**Why trading firms care:** Interviewers at HRT, Jump, Citadel and Jane Street use your project to test whether you understand what you built. They'll push on one detail until you hit the edge of your knowledge. Clear, honest answers score higher than big numbers.

## The 30-second version

> "I built a C++ engine that connects to Polymarket's live market-data WebSocket, keeps an exact order book per outcome, and streams it to my Python market maker. It records every message so any session can be replayed deterministically. I benchmarked it against a naive version on the same recorded data: about 4× lower internal latency, 1.8 µs vs 7.3 µs at the median, from switching to simdjson, a flat-array book and a lock-free queue."

## The 2-minute version: walk the diagram

1. **Network thread** stamps each frame and pushes it into a **lock-free SPSC queue**. It never parses, so slow processing can't delay socket reads.
2. **Processing thread** parses with **simdjson** into **fixed-point** integers, and updates a **flat-array book** (one slot per 0.0001 price, plus a bitmap to find the next level).
3. The feed has **no sequence numbers**, so I compare my best bid/ask to the exchange's after every update, and resync on any mismatch. Zero mismatches across every session.
4. **Publisher** writes one JSON line per book change with per-asset sequence numbers, defined in a written contract.
5. **Recorder** saves raw frames, and **replay** runs them through the same code path. The 12-minute sample reproduces the live run's final books exactly.

## Three deep-dive stories (each has a twist, which is what makes it memorable)

1. **Memory ordering is real on ARM.** "I removed acquire/release from my queue on purpose. ThreadSanitizer flagged it in every stress test, and even without TSan, on my M5 it delivered wrong values in 4 of 5 tests. On x86 it would often have looked fine."
2. **Honest benchmarking.** "My first map-vs-array benchmark showed 60×, but the map version walked every level instead of stopping at 10. Fixed, it's 2× for top-10 reads and 8× for single updates. A benchmark is only as honest as the baseline."
3. **Clocks lie.** "I assumed Python's monotonic clock matched C++'s steady_clock. On macOS it was off by about 42 minutes. I proved which clock matched by bracketing readings, and fixed the contract."

Bonus: "Live is slower than replay (≈10 µs vs 1.8 µs at p50) because messages arrive seconds apart, so the caches go cold. Replaying at only 10× reproduced part of that."

## Say this before they ask

"These are **internal** latencies on a laptop (macOS, no core pinning, 41 ns clock). I don't measure network or exchange latency, and TLS decryption happens before my timestamp. On a tuned Linux box with pinned cores and kernel-bypass networking, the numbers would differ."

## What you'd do next (shows judgement)

- Run on Linux with pinned, isolated cores, and compare.
- Keep caches warm during quiet periods (feed dummy messages through the hot path) to close the live-vs-replay gap.
- Move recording to its own thread with a second SPSC queue.
- Binary output (fixed-layout structs over shared memory) instead of JSON, if the strategy were in C++.

## Interview questions you could be asked

1. **"Why not just use `std::map` for the book?"**
   Bounded prices make an array possible. Array lookups are one memory access with good locality, while a map is pointer-chasing across scattered heap nodes. Measured: 4.9 ns vs 40.6 ns per update. A map wins when prices are unbounded or very sparse.
2. **"What happens when your connection drops?"**
   Exponential backoff with jitter, resubscribe, and the server sends fresh snapshots that replace every book. The strategy gets a `reconnecting` status, so it can pull quotes. Tested offline with a fake exchange, and live with forced disconnects.
3. **"How do you know your optimised version is correct?"**
   Differential tests against simple references (map book, nlohmann parser), the exchange's own best bid/ask on every live update, replay reproducing live state hashes, sanitizers on every build, and a protocol validator on the full output stream.
