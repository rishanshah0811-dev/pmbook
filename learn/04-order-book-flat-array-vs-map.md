# 04 · The order book: flat array vs std::map

**In one sentence:** Because Polymarket prices can only be one of 10,001 values (0.0000–1.0000), we store the book as a plain array with one slot per price, which is much faster than a tree.

**Why trading firms care:** The book is updated on every message, and read on every decision. Exchanges with bounded prices (or a known price band around the current price) almost always use array-based books, because memory layout matters more than "big-O" at this scale.

## The analogy

`std::map` is a **family tree of index cards**. To find price 0.62 you start at the root card, then follow arrows left or right through about 5–10 cards. Each card was put in a random drawer somewhere in the building (a separate heap allocation), so each step means walking to another drawer.

The flat array is **a row of 10,001 numbered pigeonholes**. Price 0.62 is pigeonhole 6200. One step, and the neighbouring pigeonholes sit right next to it.

What makes the second one fast is the **CPU cache**: a small, very fast memory close to the CPU. It loads memory in 64-byte **cache lines**. Neighbouring array slots arrive together for free. Tree nodes scattered across the heap each cost a separate trip to main memory (a **cache miss**, ~100× slower than a cache hit).

## The trick: an occupancy bitmap

Problem: when the best bid (say 6200) empties, where's the next one? Scanning 6199, 6198, … could take thousands of steps. So we also keep **one bit per price** ("is this slot non-empty?") packed into 157 64-bit words. `std::countl_zero(word)` ("count leading zeros") finds the highest set bit in one CPU instruction, so the search jumps 64 prices at a time.

We index by 0.0001 (the finest tick) rather than the market's current tick, so a `tick_size_change` needs no rebuilding.

## Keeping the book honest

Polymarket's feed has **no sequence numbers**, so we can't directly see a missed message. Instead, every `price_change` includes the exchange's `best_bid`/`best_ask` *after* the change, and we compare ours after every update. Mismatch, crossed book, or a delta with no snapshot → mark the asset `NeedsResync` and wait for a fresh snapshot. On the real 20-second capture, all 40 updates matched the exchange exactly.

(The book's `hash` field turned out not to be a simple SHA-1 of the message, and it isn't documented, so we don't rely on it.)

## Worked example: real benchmark numbers

From `results/m3_book_flat_vs_map.json` (Apple M5, clang 23, -O3 + LTO, median of 5). The workload has ~1,000 levels in the book, more than a typical live book (~40):

| Operation | Flat array | std::map | Speed-up |
|---|---|---|---|
| Update one level + read best bid/ask | **4.9 ns** | 40.6 ns | ~8× |
| Update + copy top 10 levels per side | **28.8 ns** | 56.5 ns | ~2× |
| Replace book with 30+30 snapshot | **175 ns** | 1,451 ns | ~8× |

Honesty note: my first map number for top 10 was 1,661 ns, because the map version walked *every* level instead of stopping at 10. That made the flat book look 60× better than it really is. I fixed the baseline before recording. **A benchmark is only as honest as its slowest competitor.**

Correctness: 1,000,000 random updates applied to both books; best bid/ask, level counts and sizes are compared after every update, and full state every 1,000 updates. They match every time. I also planted an off-by-one bug in the flat book to make sure the test fails (it did, on the first update).

## Interview questions you could be asked

1. **Why is a linked list (or a tree) bad for cache?**
   Each node is a separate allocation at an unpredictable address, so following a pointer is often a cache miss (~100 ns from main memory). The CPU can't prefetch what it can't predict. Arrays are contiguous, so one 64-byte cache line brings in several neighbours and the hardware prefetcher streams the rest.
2. **When would the flat array be the wrong choice?**
   When prices aren't bounded or are very sparse, like a stock trading anywhere from $1 to $5,000 in $0.01 ticks: that's 500,000 slots, mostly empty. Then you'd use an array window around the current price that re-centres, or a sorted vector, or a tree.
3. **How do you know your fast data structure is correct?**
   Differential testing: run millions of random operations against a simple, obviously-correct reference (here `std::map`) and compare after every step. Then mutate the fast code on purpose to prove the test catches bugs.
