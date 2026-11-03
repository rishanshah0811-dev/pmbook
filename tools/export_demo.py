#!/usr/bin/env python3
"""Builds docs/data.json for the browser demo from the engine's own output.

Runs `pmbook replay` on the shipped sample, keeps every PMBook `book` and
`trade` line, and compacts them (asset ids -> indexes, times relative to the
first frame). Market names come from Polymarket's public Gamma API; if that
fails, token ids are used instead.

    make release && python3 tools/export_demo.py
"""
import json
import pathlib
import subprocess
import sys
import urllib.parse
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from list_markets import tls_context  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAMMA = "https://gamma-api.polymarket.com/markets"


def market_names(assets):
    """asset_id -> (question, outcome), from one Gamma request."""
    q = urllib.parse.urlencode([("clob_token_ids", a) for a in assets])
    try:
        with urllib.request.urlopen(f"{GAMMA}?{q}", timeout=20, context=tls_context()) as r:
            markets = json.load(r)
    except Exception as e:  # offline is fine; the demo still works
        print(f"market names unavailable ({e}); using token ids", file=sys.stderr)
        return {}
    names = {}
    for m in markets:
        for outcome, token in zip(json.loads(m["outcomes"]), json.loads(m["clobTokenIds"])):
            names[token] = (m["question"], outcome)
    return names


def main() -> int:
    exe = ROOT / "build/release/pmbook"
    files = sorted(str(p) for p in (ROOT / "data/sample").glob("pm_*.jsonl.gz"))
    out = subprocess.run([str(exe), "replay", "--speed", "0", "--publish", "stdout", "--depth", "10", *files],
                         check=True, capture_output=True, text=True).stdout
    msgs = [json.loads(line) for line in out.splitlines()]

    assets = list(dict.fromkeys(m["asset_id"] for m in msgs))
    names = market_names(assets)
    # Group by market so each question's outcomes sit next to each other.
    assets.sort(key=lambda a: (names.get(a, (a, ""))[0], names.get(a, ("", ""))[1] != "Yes"))
    index = {a: i for i, a in enumerate(assets)}

    t0 = min(m["ts_recv_ns"] for m in msgs)
    rel = lambda m: round((m["ts_recv_ns"] - t0) / 1e6)  # ms since first frame
    flat = lambda levels: [x for lv in levels for x in lv]

    books = [[rel(m), index[m["asset_id"]], m["seq"], flat(m["bids"]), flat(m["asks"])]
             for m in msgs if m["type"] == "book"]
    trades = [[rel(m), index[m["asset_id"]], m["price"], m["size"], m["side"]]
              for m in msgs if m["type"] == "trade"]
    first_exchange_ms = min(m["ts_exchange_ms"] for m in msgs if m["type"] == "book")

    data = {
        "source": "pmbook replay of data/sample (recorded live 2026-11-01); depth 10",
        "start_utc_ms": first_exchange_ms,
        "duration_ms": max(b[0] for b in books),
        "markets": [{"asset_id": a, "question": names.get(a, (a[:12] + "...", ""))[0],
                     "outcome": names.get(a, ("", ""))[1]} for a in assets],
        "books": books,
        "trades": trades,
    }
    dest = ROOT / "docs/data.json"
    dest.parent.mkdir(exist_ok=True)
    dest.write_text(json.dumps(data, separators=(",", ":")))
    print(f"{dest.relative_to(ROOT)}: {len(books)} books, {len(trades)} trades, "
          f"{len(assets)} assets, {dest.stat().st_size / 1024:.0f} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
