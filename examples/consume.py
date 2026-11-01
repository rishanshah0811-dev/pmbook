#!/usr/bin/env python3
"""Example consumer for PMBook protocol v1 (see CONTRACT.md).

Reads the engine's stream and shows best bid / best ask per asset.

    # stdout mode: pipe the engine into this script
    ./build/release/pmbook live --asset ID --asset ID | python3 examples/consume.py

    # TCP mode
    ./build/release/pmbook live --asset ID --publish tcp &
    python3 examples/consume.py --tcp 127.0.0.1:9100

    # check every contract rule on a replay (exit code 1 on any violation)
    ./build/release/pmbook replay --publish stdout data/sample/pm_*.jsonl.gz \\
        | python3 examples/consume.py --validate --quiet

Standard library only.
"""
import argparse
import json
import socket
import sys
import time

def engine_clock_ns() -> int:
    """The OS clock behind the engine's C++ std::chrono::steady_clock, so
    `engine_clock_ns() - msg["ts_pub_ns"]` is a real engine->consumer delay.
    macOS (libc++): CLOCK_MONOTONIC_RAW. Linux: CLOCK_MONOTONIC.
    (On macOS, time.monotonic_ns() is a different clock: don't mix them.)"""
    if sys.platform == "darwin":
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    return time.monotonic_ns()


BOOK_KEYS = {"v", "type", "asset_id", "seq", "ts_exchange_ms", "ts_recv_ns", "ts_pub_ns", "tick", "bids", "asks"}
TRADE_KEYS = {"v", "type", "asset_id", "seq", "ts_exchange_ms", "ts_recv_ns", "price", "size", "side"}
STATUS_KEYS = {"v", "type", "state", "detail", "ts_pub_ns"}
STATES = {"connected", "reconnecting", "resync"}


class Asset:
    def __init__(self):
        self.seq = 0
        self.bid = self.ask = None  # (price, size) or None
        self.tick = None
        self.waiting_for_book = False  # after a seq gap
        self.updates = 0


class Consumer:
    def __init__(self, validate: bool, depth: int):
        self.validate = validate
        self.depth = depth
        self.assets: dict[str, Asset] = {}
        self.counts = {"book": 0, "trade": 0, "status": 0}
        self.gaps = 0
        self.errors: list[str] = []
        self.engine_latency_ns: list[int] = []  # ts_pub_ns - ts_recv_ns (live only)
        self.delivery_ns: list[int] = []  # engine publish -> this script parsed it

    def fail(self, msg: str):
        self.errors.append(msg)
        if len(self.errors) <= 10:
            print(f"CONTRACT VIOLATION: {msg}", file=sys.stderr)

    def handle(self, line: str) -> str | None:
        """Processes one line; returns a human-readable event or None."""
        try:
            m = json.loads(line)
        except json.JSONDecodeError:
            self.fail(f"not JSON: {line[:80]!r}")
            return None
        if m.get("v") != 1:
            self.fail(f"unsupported version: {m.get('v')}")
            return None
        t = m.get("type")
        if t not in self.counts:
            self.fail(f"unknown type {t!r}")
            return None
        self.counts[t] += 1
        if self.validate:
            self.check_shape(t, m)

        if t == "status":
            if m.get("state") == "reconnecting":
                for a in self.assets.values():  # everything is stale until fresh books arrive
                    a.waiting_for_book = True
            return f"[status] {m['state']}: {m['detail']}"

        a = self.assets.setdefault(m["asset_id"], Asset())
        expected = a.seq + 1
        if a.seq and m["seq"] != expected:
            self.gaps += 1
            a.waiting_for_book = True
            if self.validate:
                self.fail(f"seq gap on {m['asset_id'][:12]}: got {m['seq']}, expected {expected}")
        a.seq = m["seq"]

        if t == "trade":
            if a.waiting_for_book:
                return None  # contract: ignore trades until the next book after a gap
            return f"[trade] {m['asset_id'][:12]}... {m['side']:<4} {m['size']} @ {m['price']}"

        # book: self-contained, so it always resynchronises us
        a.waiting_for_book = False
        a.updates += 1
        a.tick = m["tick"]
        a.bid = tuple(m["bids"][0]) if m["bids"] else None
        a.ask = tuple(m["asks"][0]) if m["asks"] else None
        self.engine_latency_ns.append(m["ts_pub_ns"] - m["ts_recv_ns"])
        self.delivery_ns.append(engine_clock_ns() - m["ts_pub_ns"])
        return None

    def check_shape(self, t: str, m: dict):
        keys = {"book": BOOK_KEYS, "trade": TRADE_KEYS, "status": STATUS_KEYS}[t]
        if set(m) != keys:
            self.fail(f"{t} keys {sorted(set(m) ^ keys)} differ from contract")
            return
        if t == "status":
            if m["state"] not in STATES or not isinstance(m["detail"], str) or not isinstance(m["ts_pub_ns"], int):
                self.fail(f"bad status {m}")
            return
        aid = m["asset_id"]
        if not (isinstance(aid, str) and aid.isdigit()):
            self.fail(f"asset_id must be a digit string: {aid!r}")
        for k in ("seq", "ts_exchange_ms", "ts_recv_ns"):
            if not isinstance(m[k], int) or isinstance(m[k], bool):
                self.fail(f"{k} must be int in {t}")
        if t == "trade":
            if m["side"] not in ("BUY", "SELL") or not 0 <= m["price"] <= 1 or m["size"] <= 0:
                self.fail(f"bad trade {m}")
            return
        if not isinstance(m["ts_pub_ns"], int):
            self.fail("ts_pub_ns must be int")
        for side, desc in (("bids", True), ("asks", False)):
            lv = m[side]
            if len(lv) > self.depth:
                self.fail(f"{side} has {len(lv)} levels > depth {self.depth}")
            prices = [p for p, _ in lv]
            if prices != sorted(prices, reverse=desc) or len(set(prices)) != len(prices):
                self.fail(f"{side} not strictly sorted best-first: {prices[:5]}")
            if any(not (0 <= p <= 1) or s <= 0 for p, s in lv):
                self.fail(f"{side} has price outside [0,1] or non-positive size")
        if m["bids"] and m["asks"] and m["bids"][0][0] >= m["asks"][0][0]:
            self.fail(f"crossed book published for {aid[:12]}")

    def table(self) -> str:
        rows = [f"{'asset':<16} {'bid':>8} {'bid size':>12}   {'ask':>8} {'ask size':>12}  {'seq':>6}"]
        for aid, a in sorted(self.assets.items()):
            bid = f"{a.bid[0]:>8} {a.bid[1]:>12,.2f}" if a.bid else f"{'-':>8} {'':>12}"
            ask = f"{a.ask[0]:>8} {a.ask[1]:>12,.2f}" if a.ask else f"{'-':>8} {'':>12}"
            stale = "  (stale)" if a.waiting_for_book else ""
            rows.append(f"{aid[:12] + '...':<16} {bid}   {ask}  {a.seq:>6}{stale}")
        return "\n".join(rows)

    def summary(self) -> str:
        lat = sorted(self.engine_latency_ns)
        pct = lambda q: lat[min(len(lat) - 1, int(q * (len(lat) - 1)))] / 1000 if lat else float("nan")
        head = (f"messages: {self.counts}  assets: {len(self.assets)}  seq gaps: {self.gaps}  "
                f"contract violations: {len(self.errors)}")
        if not lat:
            return head
        if pct(0.5) > 10_000_000:  # > 10 s: this is a replay of an old recording
            return head + "\nengine latency: n/a (replay: ts_recv_ns is the recorded receive time)"
        dl = sorted(self.delivery_ns)
        dpct = lambda q: dl[min(len(dl) - 1, int(q * (len(dl) - 1)))] / 1000
        return head + (f"\nengine latency (ts_pub_ns - ts_recv_ns): "
                       f"p50 {pct(0.5):.1f} us  p99 {pct(0.99):.1f} us  max {lat[-1] / 1000:.1f} us  (n={len(lat)})"
                       f"\ndelivery to this script (pipe/TCP + Python parse): p50 {dpct(0.5):.1f} us  "
                       f"p99 {dpct(0.99):.1f} us")


def lines_from_tcp(addr: str):
    host, port = addr.rsplit(":", 1)
    with socket.create_connection((host, int(port))) as s:
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for ln in lines:
                yield ln.decode()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tcp", metavar="HOST:PORT", help="connect to the engine's TCP stream instead of stdin")
    ap.add_argument("--validate", action="store_true", help="check every contract rule; exit 1 on violation")
    ap.add_argument("--depth", type=int, default=10, help="depth the engine was started with")
    ap.add_argument("--quiet", action="store_true", help="only print the final summary")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this many seconds")
    args = ap.parse_args()

    c = Consumer(args.validate, args.depth)
    source = lines_from_tcp(args.tcp) if args.tcp else (ln.rstrip("\n") for ln in sys.stdin)
    tty = sys.stdout.isatty()
    start = last_draw = time.monotonic()
    try:
        for line in source:
            if not line:
                continue
            event = c.handle(line)
            now = time.monotonic()
            if args.quiet:
                pass
            elif tty:
                if event and event.startswith("[status]"):
                    print(event)
                if now - last_draw > 0.25:
                    print("\033[2J\033[H" + c.table(), flush=True)  # clear screen + redraw
                    last_draw = now
            else:
                if event:
                    print(event, flush=True)
                if now - last_draw > 2.0:  # a periodic table when piped/logged
                    print(c.table(), "\n", flush=True)
                    last_draw = now
            if args.seconds and now - start > args.seconds:
                break
    except KeyboardInterrupt:
        pass
    if not args.quiet:
        print(c.table())
    print(c.summary())
    return 1 if (args.validate and c.errors) or sum(c.counts.values()) == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
