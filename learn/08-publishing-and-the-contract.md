# 08 · Publishing: the contract with the strategy

**In one sentence:** After every book change, the engine writes one line of JSON describing the top of the book, in a format written down in `CONTRACT.md`, so a separate program (your Python market maker) can trust and use it.

**Why trading firms care:** Firms are built from many processes: feed handlers, strategies, risk, order gateways. Each pair talks over a written interface. When the interface is vague ("what does a gap mean?", "is this sorted?", "which clock is that?"), you get bugs that only show up live, with money on the line.

## The analogy

A restaurant kitchen (engine) and waiters (strategy). The **ticket format** is the contract: table number, dish, time, in that order, every time. Sequence numbers are the ticket numbers. If a waiter sees ticket 41 and then 43, they know one went missing. Here they then wait for the next "full table summary" (a `book` message), which is complete on its own, so nothing stays lost.

## What we built

- **Messages**: `book` (top N levels after every change), `trade`, `status` (`connected` / `reconnecting` / `resync`).
- **`seq` per asset**, shared by books and trades, with no gaps. Every `book` is self-contained, so after a gap the consumer just waits for the next one.
- **Transports**: stdout (pipe into Python) or TCP on 127.0.0.1:9100. TCP sockets are non-blocking, so a slow client can't freeze the engine. A client that falls 8 MB behind is dropped, and a client joining late first gets the latest book of every asset.
- **Status in order**: "connected" / "reconnecting" travel through the same lock-free queue as the market data, so the strategy sees them in the right place in the stream.
- **`ts_pub_ns` is stamped after formatting**, just before writing, so the latency includes JSON formatting.

## A wrong claim, caught by testing

I first wrote in the contract that Python's `time.monotonic_ns()` uses the same clock as the engine. Measured on your Mac, it was off by a constant ~42 minutes. On macOS, C++ `steady_clock` is `CLOCK_MONOTONIC_RAW`; Python's monotonic is a different clock (the gap is likely time spent asleep). I proved which one matches (the C++ reading fell inside a Python before/after window 5 of 5 times), fixed the contract, and added `engine_clock_ns()` to the example consumer. Lesson: **"same clock" is a claim; test it.**

## Worked example (real output, 40 s live, 6 tokens)

```
$ pmbook live ... | python3 examples/consume.py --validate --quiet
messages: {'book': 142, 'trade': 0, 'status': 1}  assets: 6  seq gaps: 0  contract violations: 0
engine latency (ts_pub_ns - ts_recv_ns): p50 54.1 us  p99 239.8 us
delivery to this script (pipe/TCP + Python parse): p50 103.4 us  p99 192.2 us
```

Why is engine latency ~50 µs when the book update itself takes ~5 ns? With only a few messages a second, the processing thread naps between them (`--wait hybrid`) and has to wake up. Milestone 8 separates those costs properly.

The full 12-minute replay (2,938 books, 3 trades) passes `--validate` with zero violations, and that check runs in `ctest` on every build.

## Interview questions you could be asked

1. **Your consumer sees seq 41 then 43 for an asset. What should it do?**
   Treat that asset as stale: stop using it for decisions and ignore its trades until the next full snapshot (`book`) arrives, then resume from that message's seq. That's why snapshots are self-contained.
2. **Why non-blocking sockets for publishing?**
   A blocking `send` to a slow consumer would stall the engine thread and delay every other consumer and the book itself. Non-blocking sends buffer the backlog and drop clients that fall too far behind, so one bad client can't hurt everyone.
3. **How do you measure latency across two processes?**
   Both must read the same monotonic clock on the same machine, and you must verify they really are the same clock. Across machines you need clock sync (PTP) or hardware timestamps; comparing raw timestamps from two hosts measures clock offset as much as latency.
