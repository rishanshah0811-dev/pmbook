#!/bin/sh
# Runs every milestone-8 benchmark on the shipped sample and writes results/.
# Run on an otherwise idle machine (close other apps; don't compile meanwhile).
#   sh tools/run_benchmarks.sh
set -e
B=./build/release/bench/pm_latency_bench
DATA="data/sample/pm_20261101_11.jsonl.gz data/sample/pm_20261101_12.jsonl.gz"
EXP=data/sample/live_summary.json
# Latency: replay at 1000x real time (avg gap ~0.5 ms), 200 loops = 306,600 frames.
$B --variant naive     --wait spin   --speed 1000 --loops 200 --expect $EXP --out results/m8_replay_naive_spin.json $DATA
$B --variant optimized --wait spin   --speed 1000 --loops 200 --expect $EXP --out results/m8_replay_optimized_spin.json $DATA
$B --variant optimized --wait hybrid --speed 1000 --loops 200 --expect $EXP --out results/m8_replay_optimized_hybrid.json $DATA
# Throughput: back-to-back processing, one thread.
$B --variant naive     --throughput --loops 50  --expect $EXP --out results/m8_throughput_naive.json $DATA
$B --variant optimized --throughput --loops 200 --expect $EXP --out results/m8_throughput_optimized.json $DATA
# Cold-cache experiment: same data at only 10x (avg gap ~50 ms), one loop.
$B --variant optimized --wait spin   --speed 10   --loops 1   --expect $EXP --out results/m8_replay_optimized_spin_10x.json $DATA
