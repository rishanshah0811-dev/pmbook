#!/usr/bin/env python3
"""Builds the latency chart and the results table from results/m8_*.json.

Every number in the output comes from those files (written by
pm_latency_bench and `pmbook live --latency-out`); nothing is typed in.

    python3 tools/plot_latency.py            # -> results/m8_latency.png, results/m8_summary.md
"""
import json
import pathlib
import sys

RESULTS = pathlib.Path(__file__).resolve().parent.parent / "results"

RUNS = [  # (file, label) in display order
    ("m8_replay_naive_spin.json", "Replay: naive (nlohmann + std::map + mutex)"),
    ("m8_replay_optimized_spin.json", "Replay: optimised (simdjson + flat + SPSC)"),
    ("m8_replay_optimized_hybrid.json", "Replay: optimised, hybrid wait"),
    ("m8_replay_optimized_spin_10x.json", "Replay at 10x: optimised (colder caches)"),
    ("m8_live_spin.json", "Live: optimised, spin"),
    ("m8_live_hybrid.json", "Live: optimised, hybrid wait"),
]


def load(name):
    p = RESULTS / name
    return json.loads(p.read_text()) if p.exists() else None


def us(ns):
    return ns / 1000.0


def table() -> str:
    lines = []
    machine = build = None
    lines.append("## Internal latency: frame received -> book updated -> PMBook line published\n")
    lines.append("| Run | frames measured* | p50 | p90 | p99 | p99.9 | max | service p50 | service p99 |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for f, label in RUNS:
        d = load(f)
        if not d:
            continue
        machine, build = d["machine"], d["build"]
        h, s = d["internal_latency"], d["service_time"]
        lines.append(
            f"| {label} | {h['count']:,} | {us(h['p50_ns']):.1f} µs | {us(h['p90_ns']):.1f} µs | "
            f"{us(h['p99_ns']):.1f} µs | {us(h['p99_9_ns']):.1f} µs | {us(h['max_ns']):.0f} µs | "
            f"{us(s['p50_ns']):.2f} µs | {us(s['p99_ns']):.2f} µs |")
    lines.append("")
    lines.append("\\* Frames that produced at least one published message (PONGs and frames for "
                 "assets not yet snapshotted are excluded). Replay runs: the 12-minute shipped sample, "
                 "1,533 frames, replayed 200x at 1000x real-time gaps. Replay publishes into a sink that "
                 "discards bytes; live runs write to a stdout pipe read by examples/consume.py.\n")
    lines.append("## Throughput: processing only, one thread, no pacing\n")
    lines.append("| Variant | frames | frames/s | µs per frame | books match live run |")
    lines.append("|---|---:|---:|---:|---|")
    tps = {}
    for v in ("naive", "optimized"):
        d = load(f"m8_throughput_{v}.json")
        if not d:
            continue
        tps[v] = d["frames_per_s"]
        lines.append(f"| {d['stack']} | {d['frames']:,} | {d['frames_per_s']:,.0f} | "
                     f"{1e6 / d['frames_per_s']:.2f} | {d['books_match_live_run']} |")
    lines.append("")
    n, o = load("m8_replay_naive_spin.json"), load("m8_replay_optimized_spin.json")
    if n and o:
        lines.append("## Speed-ups (same data, same machine)\n")
        for k, name in (("p50_ns", "p50"), ("p99_ns", "p99"), ("p99_9_ns", "p99.9")):
            lines.append(f"- internal latency {name}: {n['internal_latency'][k] / o['internal_latency'][k]:.1f}x lower")
        lines.append(f"- service time p50: {n['service_time']['p50_ns'] / o['service_time']['p50_ns']:.1f}x lower")
        if len(tps) == 2:
            lines.append(f"- throughput: {tps['optimized'] / tps['naive']:.1f}x higher")
        lines.append("")
    if machine:
        lines.append(f"Machine: {machine}  \nBuild: {build}  \n")
    return "\n".join(lines)


def plot(out: pathlib.Path) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping chart (pip install matplotlib)", file=sys.stderr)
        return False
    fig, ax = plt.subplots(figsize=(9, 5.2))
    styles = {"naive": ("#c0392b", "-"), "optimized_spin": ("#1f6feb", "-"), "optimized_hybrid": ("#1f6feb", "--"),
              "optimized_10x": ("#8e44ad", ":"),
              "live_spin": ("#2e7d32", "-"), "live_hybrid": ("#2e7d32", "--")}
    keys = ["naive", "optimized_spin", "optimized_hybrid", "optimized_10x", "live_spin", "live_hybrid"]
    for (f, label), key in zip(RUNS, keys):
        d = load(f)
        if not d:
            continue
        b = d["internal_latency"]["buckets"]
        total = sum(c for _, _, c in b)
        # Percentile curve: x = latency (bucket upper edge), y = fraction of frames slower.
        xs, ys, seen = [], [], 0
        for lo, hi, c in b:
            seen += c
            xs.append(max(hi, 1) / 1000.0)
            ys.append(max(1 - seen / total, 1 / total / 10))
        color, ls = styles[key]
        ax.step(xs, ys, where="post", color=color, linestyle=ls, linewidth=1.8, label=f"{label} (n={total:,})")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("internal latency: received → published (µs, log scale)")
    ax.set_ylabel("fraction of frames slower than x")
    for q, name in ((0.5, "p50"), (0.01, "p99"), (0.001, "p99.9")):
        ax.axhline(q, color="#999", linewidth=0.6)
        ax.annotate(name, xy=(1.0, q), xycoords=("axes fraction", "data"), xytext=(4, -3),
                    textcoords="offset points", fontsize=8, color="#666")
    ax.grid(True, which="major", alpha=0.25)
    any_d = next((load(f) for f, _ in RUNS if load(f)), None)
    sub = any_d["machine"] if any_d else ""
    ax.set_title("pmbook internal latency (lower and further left is better)\n" + sub, fontsize=10)
    ax.legend(fontsize=8, loc="lower left")
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    return True


def main() -> int:
    md = "# pmbook benchmark results (generated by tools/plot_latency.py)\n\n" + table()
    (RESULTS / "m8_summary.md").write_text(md)
    print(md)
    if plot(RESULTS / "m8_latency.png"):
        print(f"chart: {RESULTS / 'm8_latency.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
