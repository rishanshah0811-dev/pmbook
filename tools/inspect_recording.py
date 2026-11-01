#!/usr/bin/env python3
"""Summarise recorded pm_*.jsonl.gz files: frames, message types, duration,
gaps between frames, and whether every line matches the recording contract.

    python3 tools/inspect_recording.py data/recordings/pm_*.jsonl.gz
"""
import collections
import gzip
import json
import sys


def read_lines(path: str, unfinished: list[str]):
    """Yields lines; tolerates a file that is still being written (or whose
    writer crashed): everything up to the last sync flush is returned."""
    try:
        with gzip.open(path, "rt", encoding="utf-8") as f:
            for line in f:
                if line.endswith("\n"):
                    yield line
    except EOFError:
        unfinished.append(path)


def main(paths: list[str]) -> int:
    counts = collections.Counter()
    first_ns = last_ns = None
    max_gap = (0, None)
    bad = 0
    lines = 0
    raw_bytes = 0
    unfinished = []
    for path in sorted(paths):
        for line in read_lines(path, unfinished):
            lines += 1
            rec = json.loads(line)
            if set(rec) != {"ts_recv_ns", "raw"} or not isinstance(rec["ts_recv_ns"], int):
                bad += 1
                continue
            ts, raw = rec["ts_recv_ns"], rec["raw"]
            raw_bytes += len(raw.encode())
            if last_ns is not None and ts - last_ns > max_gap[0]:
                max_gap = (ts - last_ns, ts)
            if last_ns is not None and ts < last_ns:
                counts["!non_monotonic_ts"] += 1
            first_ns = ts if first_ns is None else first_ns
            last_ns = ts
            if raw == "PONG":
                counts["PONG"] += 1
                continue
            j = json.loads(raw)
            for item in j if isinstance(j, list) else [j]:
                counts[item.get("event_type", "?")] += 1
    span = (last_ns - first_ns) / 1e9 if lines else 0
    print(f"files: {len(paths)}  lines: {lines}  bad lines: {bad}")
    for u in unfinished:
        print(f"  note: {u} has no gzip end marker (still being written, or writer crashed)")
    print(f"span: {span:.1f} s  raw bytes: {raw_bytes:,}  largest gap: {max_gap[0] / 1e9:.2f} s")
    for k, v in counts.most_common():
        print(f"  {k:>20}: {v}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
