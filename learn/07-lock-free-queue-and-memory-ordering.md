# 07 · The lock-free queue and memory ordering

**In one sentence:** One thread reads the network and drops each message into a ring buffer, another thread picks them up and updates the books, and because exactly one thread writes each end, they can share it with no lock at all.

**Why trading firms care:** A lock can put a thread to sleep, and waking up costs microseconds: forever in trading. SPSC queues are everywhere in low-latency systems (feed handler → strategy, strategy → order gateway), and "explain acquire/release" is one of the most common C++ interview questions at HFT firms.

## The analogy

A **conveyor belt sushi bar** with 8 numbered plates in a loop. The chef (producer) only puts food on plates; the customer (consumer) only takes it off. The chef never touches the customer's counter, and vice versa, so they never need to take turns with a key (lock).

The one rule: the chef must **finish** arranging the sushi before moving the "ready up to plate 5" sign. And the customer must look at the sign **before** grabbing plate 5. In C++:
- the producer's sign update is a **release** store: everything I did before this is visible to anyone who sees it;
- the consumer's sign check is an **acquire** load: once I see the sign, I see everything done before it.

Without that pairing, the CPU or compiler may **reorder**. The sign could move before the plate is ready, and the customer grabs half a meal.

## We proved it matters, on this Mac

I rebuilt the queue with the pairing removed (`memory_order_relaxed`):

| Build | Result |
|---|---|
| Correct queue, ThreadSanitizer, 10 rounds | 0 warnings, all pass |
| Broken, ThreadSanitizer | race reported in **all 5** stress tests |
| Broken, plain -O3, no sanitizer | **wrong values in 4 of 5 tests, every run**, e.g. "got 1986, expected 1988" |

Apple Silicon (ARM) really does reorder memory operations. On x86 the broken version would often *seem* to work, which is exactly why these bugs survive testing. ThreadSanitizer catches them even when the values happen to come out right.

## Other tricks in the queue

- **False sharing**: the producer's index and the consumer's index sit on different 128-byte cache lines (Apple Silicon moves memory in 128-byte blocks between some cache levels). On one shared line, every write by one core would invalidate the other core's copy.
- **Cached indices**: each side keeps a private copy of the other's index and only re-reads the shared one when its copy says "full" or "empty".
- **Two-phase push**: the producer writes straight into the slot's reused `std::string` buffer, so there's no allocation per message.
- **Wait strategy**: `--wait spin` burns a core for the lowest latency; `hybrid` (the default) naps after 50 µs idle. That's cheap on a laptop, but a message after a quiet spell pays the wake-up time.

## Worked example: real numbers

From `results/m6_queue_bench.json` (Apple M5, -O3 + LTO, 2 threads spinning, median of 5, no core pinning on macOS, clock resolution 41 ns):

| | SPSC lock-free | std::mutex queue |
|---|---|---|
| Throughput | **200 M items/s** | 38 M items/s |
| Round trip p50 | **125 ns** | 3,583 ns |
| Round trip p99.9 | **375 ns** | 34,625 ns |

Honesty notes: SPSC throughput swung between 135 and 302 M/s across runs because macOS moves threads between fast and efficiency cores. And timings are multiples of the 41 ns clock tick.

A bug I made and found: a test called `pop()` without `front()` first, breaking the queue's protocol, and the test failed. The queue now `assert`s that protocol in debug builds, and a test checks that the assert fires.

## Interview questions you could be asked

1. **What is false sharing?**
   Two threads write *different* variables that happen to live on the same cache line. The line bounces between cores as each write invalidates the other's copy, so it's slow even though no data is actually shared. Fix: align/pad each thread's hot data to its own cache line.
2. **Explain acquire/release in an SPSC queue.**
   The producer writes the data, then stores the new tail with `release`. The consumer loads the tail with `acquire` and then reads the data. The pair guarantees the data writes happen-before the reads. With `relaxed`, the CPU/compiler may make the tail visible first, and the consumer reads a half-written slot.
3. **Why is SPSC lock-free but MPMC much harder?**
   With one producer and one consumer, each index has a single writer, so plain atomic loads/stores suffice. With multiple producers, two threads can claim the same slot, so you need compare-and-swap loops and per-slot sequence numbers, and you face contention and the ABA problem.
