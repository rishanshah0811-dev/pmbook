# 09 · Measuring latency honestly

**In one sentence:** We timed every message from "arrived from the socket" to "book updated and line published", on the same recorded data for the naive and the optimised engine, and we report the whole distribution, not just the average.

**Why trading firms care:** In trading, the *slow* messages hurt: a quote that's 1 ms stale at the wrong moment gets picked off. So firms quote percentiles (p50, p99, p99.9, max), and an interviewer will immediately ask "what exactly did you measure, on what machine, and what didn't you measure?"

## Key words

- **Latency**: how long one message takes. **Throughput**: how many messages per second.
- **p99** = 99% of messages were faster than this. **p99.9** = 1 in 1,000 was slower. The average hides the tail.
- **Internal latency**: from our network thread's timestamp to "published" (queue wait + wake-up + processing). **Service time**: just the processing.

## The analogy

A coffee shop. The **service time** is how long the barista takes per drink. Your **latency** also includes waiting in the queue and the barista looking up from their phone (waking up). A shop can have a fast barista and still a slow p99, if drinks arrive in bursts or the barista naps between customers.

## What we measured (and what we didn't)

- ✅ **Measured**: inside our process, from just after IXWebSocket hands us a frame to just after the PMBook line is handed to the sink.
- ❌ **Not measured**: network time to Polymarket, TLS decryption and WebSocket framing inside IXWebSocket, Polymarket's own matching delay, and the strategy's reaction. Comparing `ts_exchange_ms` with our clock would mix in clock differences between machines, so we never call it network latency.
- Replay runs publish into a sink that discards the bytes; writing to a pipe adds about 1 µs (we measured it).

## Results (Apple M5, `results/m8_summary.md`, generated from the JSON files)

Same 12-minute recording, replayed 200 times at 1,000× real-time gaps (293,000 measured frames):

| | p50 | p99 | p99.9 | throughput |
|---|---:|---:|---:|---:|
| Naive (nlohmann + std::map + mutex) | 7.3 µs | 57.3 µs | 103.4 µs | 214k frames/s |
| **Optimised** (simdjson + flat + SPSC) | **1.8 µs** | **13.6 µs** | **28.2 µs** | **660k frames/s** |

That's about **4× lower latency at p50/p99** and **3.1× the throughput**. Both produce identical books, checked against the live run's fingerprints.

Live, 10 minutes, 6 tokens, busy-spin: p50 9.7 µs, p99 27.1 µs, p99.9 104.4 µs, max 177 µs.

## Three things the numbers taught us

1. **Napping costs ~30 µs.** Hybrid wait (sleep when idle) moved replay p50 from 1.8 µs to 29.2 µs. Spin costs a full CPU core; that's the trade-off.
2. **Cold caches.** Live service time is ~8 µs, versus 1.5 µs in fast replay. Replaying the same data at only 10× (gaps ~50 ms) raised service time to 3.7 µs. So the gap is consistent with caches and branch predictors going cold between messages that arrive seconds apart, plus ~1 µs for the stdout write. Real HFT systems "keep warm" by running dummy messages through the hot path.
3. **Benchmark hygiene.** My first live spin run showed a 2.5 ms tail, but I had been compiling code at the same time. Its own service time never exceeded 0.4 ms, so the stall was the OS pausing our thread. Re-run on an idle machine: max 177 µs. The first run is kept in `data/recordings/m8/`, but not quoted.

## Interview questions you could be asked

1. **Why report p99.9 and max rather than the mean?**
   Latency distributions are skewed with long tails. The mean hides rare slow events, which are exactly the costly ones (stale quotes during bursts). Percentiles show the tail; max shows the worst case you actually hit.
2. **What is "coordinated omission"?**
   If your load generator waits for each response before sending the next, a stall delays the *sending* too, so the stall shows up once instead of in every message that should have been sent. We avoid it by pacing frames on a fixed schedule from the recording and stamping each when it's *due*, so queueing delay counts.
3. **Your benchmark is 4× faster. How do you know it's not measuring something wrong?**
   Same input data, same machine, same code path except the three swapped components. Both variants must produce identical final books. The clock resolution is known (41 ns). The machine was idle. And a run that was disturbed (compiling) was thrown out, with the reason written down.
