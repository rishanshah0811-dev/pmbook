# Sample recording

12 minutes of real Polymarket market-channel data, recorded by `pmbook live`
on 2026-11-01 from about 11:54 to 12:06 UTC (720 s run).

- 3 markets / 6 tokens (`assets.txt`), chosen by `tools/list_markets.py`
- 1,533 frames: 1,451 price_change, 36 book, 3 last_trade_price, 68 PONG
- 5 connections: 3 forced disconnects (`--chaos-disconnect 180`) plus 1 real
  drop from the network; every reconnect resubscribed and got fresh snapshots
- Rotated live across the 12:00 UTC boundary, so there are two files
- `live_summary.json` is the live run's final book state per asset; replaying
  these files must reproduce the same `state_hash` values (milestone 5)
