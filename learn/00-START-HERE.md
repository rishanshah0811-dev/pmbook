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

More lessons get added as each milestone lands. Terms are defined in [GLOSSARY.md](GLOSSARY.md).
