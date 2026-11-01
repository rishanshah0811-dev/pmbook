# Start here

This folder is your course notes for the engine. Read the lessons in order. Each one is a page and ends with interview questions.

## The big picture

```
   Polymarket servers
          │  WebSocket (TLS), JSON text frames
          ▼
 ┌───────────────────┐   raw frame + receive time (ns)
 │ feed/  (network   │──────────────────────────────┐
 │  thread)          │                              ▼
 └─────────┬─────────┘                     ┌──────────────────┐
           │ push into lock-free ring      │ record/          │
           ▼   (SPSC queue)                │ pm_YYYYMMDD_HH   │
 ┌───────────────────┐                     │ .jsonl.gz        │
 │ parse/ (simdjson  │                     └────────┬─────────┘
 │ → fixed-point     │◄──── replay/ reads files ────┘
 │ structs)          │      (same path, offline)
 └─────────┬─────────┘
           ▼
 ┌───────────────────┐
 │ book/ flat array  │  best bid/ask, depth, microprice,
 │ per token         │  hash check → resync on mismatch
 └─────────┬─────────┘
           ▼
 ┌───────────────────┐   PMBook protocol v1 (NDJSON)
 │ publish/          │──── stdout or TCP 127.0.0.1:9100 ───► Python market maker
 └───────────────────┘
        bench/ measures time from "frame received" to "published"
```

## Lessons

| # | Lesson | Milestone |
|---|--------|-----------|
| 01 | [What is a market data feed and an order book?](01-market-data-feed-and-order-book.md) | before M1 |
| 02 | [How a C++ project is built: CMake, dependencies, sanitizers](02-building-cpp-cmake-sanitizers.md) | M1 |
| 03 | [Parsing messages and why prices are integers](03-parsing-and-fixed-point.md) | M2 |
| 04 | [The order book: flat array vs std::map](04-order-book-flat-array-vs-map.md) | M3 |
| 05 | [The live feed: reconnects, heartbeats and recording](05-live-feed-reconnects-and-recording.md) | M4 |
| 06 | [Replay: running yesterday again, exactly](06-replay-and-determinism.md) | M5 |
| 07 | [The lock-free queue and memory ordering](07-lock-free-queue-and-memory-ordering.md) | M6 |
| 08 | [Publishing: the contract with the strategy](08-publishing-and-the-contract.md) | M7 |
| 09 | [Measuring latency honestly](09-measuring-latency-honestly.md) | M8 |
| 10 | [Explaining this project in an interview](10-explaining-this-project-in-an-interview.md) | M9 |

Terms are defined in [GLOSSARY.md](GLOSSARY.md). The protocol is in [../CONTRACT.md](../CONTRACT.md), and results are in [../results/](../results/).
