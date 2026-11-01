#!/usr/bin/env python3
"""List active, liquid Polymarket markets and their CLOB token IDs.

Uses the public Gamma API (no auth). Standard library only.

    python3 tools/list_markets.py --limit 10
    python3 tools/list_markets.py --limit 3 --ids-only   # space-separated token IDs
"""
import argparse
import json
import os
import ssl
import sys
import urllib.parse
import urllib.request

GAMMA = "https://gamma-api.polymarket.com/markets"


def tls_context() -> ssl.SSLContext:
    """Verified TLS context. python.org builds on macOS ship without root
    certificates, so fall back to certifi or the system bundle."""
    try:
        import certifi  # type: ignore
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    if os.path.exists("/etc/ssl/cert.pem"):
        return ssl.create_default_context(cafile="/etc/ssl/cert.pem")
    return ssl.create_default_context()


def fetch(limit: int, min_liquidity: float) -> list[dict]:
    params = {
        "active": "true",
        "closed": "false",
        "order": "volume24hr",
        "ascending": "false",
        "limit": str(limit * 3),  # over-fetch, then filter
    }
    url = f"{GAMMA}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": "pmbook/0.1"})
    with urllib.request.urlopen(req, timeout=15, context=tls_context()) as resp:
        markets = json.load(resp)
    out = []
    for m in markets:
        if not m.get("enableOrderBook"):
            continue
        if float(m.get("liquidityNum") or m.get("liquidity") or 0) < min_liquidity:
            continue
        # clobTokenIds and outcomes arrive as JSON-encoded strings.
        tokens = json.loads(m.get("clobTokenIds") or "[]")
        outcomes = json.loads(m.get("outcomes") or "[]")
        if len(tokens) != len(outcomes) or not tokens:
            continue
        out.append({
            "question": m.get("question", ""),
            "volume24hr": float(m.get("volume24hr") or 0),
            "liquidity": float(m.get("liquidityNum") or m.get("liquidity") or 0),
            "tick": m.get("orderPriceMinTickSize"),
            "tokens": list(zip(outcomes, tokens)),
        })
        if len(out) == limit:
            break
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--limit", type=int, default=10)
    ap.add_argument("--min-liquidity", type=float, default=10_000)
    ap.add_argument("--ids-only", action="store_true", help="print token IDs only")
    args = ap.parse_args()

    markets = fetch(args.limit, args.min_liquidity)
    if args.ids_only:
        print(" ".join(tok for m in markets for _, tok in m["tokens"]))
        return 0
    for m in markets:
        print(f"{m['question']}")
        print(f"  24h vol ${m['volume24hr']:,.0f}  liquidity ${m['liquidity']:,.0f}  tick {m['tick']}")
        for outcome, tok in m["tokens"]:
            print(f"  {outcome:>4}: {tok}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
