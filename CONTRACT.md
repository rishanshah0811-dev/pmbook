# PMBook protocol v1

The interface between `pmbook` (this C++ engine) and downstream consumers, such as the Python market-making strategy. Anything not written here is not part of the contract.

## Transport

| Mode | How | Flag |
|---|---|---|
| stdout (default) | Newline-delimited JSON on the engine's stdout. Logs and stats go to stderr, never stdout. | `--publish stdout` |
| TCP | The engine listens on `127.0.0.1:9100` (localhost only) and streams the same lines to every connected client. | `--publish tcp [--publish-port N]` |

- One JSON object per line, UTF-8, terminated by `\n`. No pretty-printing.
- TCP: a client that connects mid-stream first receives the **most recent `book` message for every asset** (an exact copy, same `seq`), then the live stream. A client that can't keep up (more than 8 MB unsent) is disconnected; it can reconnect.

## Messages

Every message has `"v": 1` and a `"type"`. Consumers must ignore fields they don't know (future minor additions), but `v` changes only for breaking changes.

### `book`: top of book after a change

```json
{"v":1,"type":"book","asset_id":"8324...5563","seq":42,"ts_exchange_ms":1793534781340,
 "ts_recv_ns":5634881223145,"ts_pub_ns":5634881239012,"tick":0.01,
 "bids":[[0.94,1520.5],[0.93,800]],"asks":[[0.95,310],[0.96,1200]]}
```

| Field | Type | Meaning |
|---|---|---|
| `asset_id` | string | Polymarket token ID (decimal digits, too big for a 64-bit int: keep it as a string) |
| `seq` | int | Per-asset sequence number (see below) |
| `ts_exchange_ms` | int | Exchange timestamp of the message that caused this update (Unix ms) |
| `ts_recv_ns` | int | Engine `steady_clock` time when the frame arrived from the socket (ns) |
| `ts_pub_ns` | int | Engine `steady_clock` time just before this line was written (ns) |
| `tick` | number | Current minimum price increment (e.g. 0.01, 0.001) |
| `bids` | `[[price, size], ...]` | Up to N levels (default 10, `--depth N`), **best (highest) first** |
| `asks` | `[[price, size], ...]` | Up to N levels, **best (lowest) first** |

- Sent on **every** change to that asset's book: after a snapshot, and after each exchange message containing deltas for that asset (several deltas in one exchange message produce one `book`).
- Each `book` is self-contained: it replaces the consumer's previous view of that asset.
- Prices are in [0, 1]; sizes are shares. Values are exact decimals: the engine stores them as integers internally (price in 1e-4, size in 1e-6), and prints the shortest exact decimal.
- An empty side is `[]`.

### `trade`

```json
{"v":1,"type":"trade","asset_id":"8324...5563","seq":43,"ts_exchange_ms":1793534781512,
 "ts_recv_ns":5634881400001,"price":0.95,"size":120,"side":"BUY"}
```

`side` is the aggressor side as reported by Polymarket (`BUY` or `SELL`). A trade doesn't itself change the published book; the book change arrives as its own `book` message.

### `status`

```json
{"v":1,"type":"status","state":"reconnecting","detail":"connection lost (forced disconnect)","ts_pub_ns":5634990000000}
```

| `state` | Meaning | What a consumer should do |
|---|---|---|
| `connected` | (Re)connected and subscribed; fresh snapshots follow | Wait for a `book` per asset before trusting it |
| `reconnecting` | Connection lost; data is stale until `connected` + new books | Treat every book as stale; e.g. pull quotes |
| `resync` | One asset's book failed a consistency check (`detail` names the asset and reason) | Treat that asset as stale until its next `book` |

## Sequence numbers

- `seq` is **per asset**, starts at 1, and increases by exactly 1 for each `book` or `trade` message for that asset (both types share one counter).
- A gap (receiving `seq` other than last+1) means the consumer missed messages. Ignore that asset's `trade` messages until the next `book` arrives, then continue from its `seq`. Since every `book` is a full top-N view, no data is lost after it.

## Timestamps: what they are and aren't

- `ts_recv_ns` and `ts_pub_ns` come from the engine's C++ `std::chrono::steady_clock`. They are comparable **with each other** (`ts_pub_ns - ts_recv_ns` = engine processing latency, including queueing). They are **not** wall-clock time.
- To compare them with a consumer's own clock on the same machine, read the same OS clock: on **macOS** that's `CLOCK_MONOTONIC_RAW` (Python: `time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)`); on **Linux** it's `CLOCK_MONOTONIC` (Python: `time.monotonic_ns()`). Verified on macOS: Python's `time.monotonic_ns()` is a *different* clock there and was off by about 42 minutes (likely time spent asleep). `examples/consume.py` has an `engine_clock_ns()` helper.
- `ts_exchange_ms` is the exchange's wall-clock timestamp. Comparing it to local wall time mixes in clock offset between machines, so it is **not** a network latency measurement.

## Recording format (raw input, for replay)

- Files: `pm_<YYYYMMDD_HH>.jsonl.gz` (UTC hour), gzip-compressed JSON Lines, rotated hourly.
- Each line: `{"ts_recv_ns":<int>,"raw":<original WebSocket text frame as a JSON string>}`. Every received frame is recorded, including `PONG`.
- `ts_recv_ns` uses the same clock as in the messages above.

## Example consumer

`examples/consume.py` implements this contract, prints best bid/ask, and with `--validate` checks every rule above.
