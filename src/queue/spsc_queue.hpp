#pragma once
// Lock-free single-producer / single-consumer (SPSC) ring buffer.
//
// Exactly ONE thread may push and exactly ONE (other) thread may pop. With
// that restriction we need no locks: each index has a single writer.
//
//   tail_  next slot to write. Written only by the producer.
//   head_  next slot to read.  Written only by the consumer.
//
// Memory ordering, in one paragraph: CPUs and compilers may reorder memory
// operations. The producer writes the slot, THEN publishes tail_ with a
// *release* store, which means "every write I did before this is visible to
// whoever sees this store". The consumer reads tail_ with an *acquire* load,
// which means "after I see this value, I also see everything written before
// it". So when the consumer sees tail_ = 5, slot 4 is guaranteed complete.
// The same pairing on head_ tells the producer that a slot has been fully
// read and may be overwritten. Without it, the consumer could read a
// half-written slot. (We prove ThreadSanitizer catches exactly that.)
//
// Two classic optimisations:
//  * The producer and consumer fields sit on separate cache lines. If they
//    shared one, every write by one core would invalidate the other core's
//    copy of the line ("false sharing"), even though they touch different
//    variables.
//  * Each side keeps a private cached copy of the other side's index and
//    only re-reads the shared atomic when the cache says full/empty. Most
//    operations then touch no shared cache line at all.
//
// Indices only ever increase (wrapping after 2^64 pushes, which would take
// centuries); slot = index & mask, so capacity must be a power of two.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pm {

// Apple Silicon moves memory in 128-byte blocks between some cache levels,
// most other CPUs use 64. Aligning to 128 is safe on both.
inline constexpr std::size_t kCacheLine = 128;

template <class T>
class SpscQueue {
#ifdef PM_SPSC_BROKEN_ORDERING
  // DEMO ONLY (learn/07): drops the acquire/release pairing to show that
  // ThreadSanitizer reports the resulting race. Never define in real builds.
  static constexpr auto kAcquire = std::memory_order_relaxed;
  static constexpr auto kRelease = std::memory_order_relaxed;
#else
  static constexpr auto kAcquire = std::memory_order_acquire;
  static constexpr auto kRelease = std::memory_order_release;
#endif

 public:
  explicit SpscQueue(std::size_t capacity) : slots_(capacity), mask_(capacity - 1) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("SpscQueue capacity must be a power of two >= 2");
    }
  }
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  std::size_t capacity() const noexcept { return slots_.size(); }

  // ---- Producer side (one thread only) --------------------------------------

  // Two-phase push: get the next free slot, fill it in place, then commit.
  // Filling in place lets a std::string slot reuse its buffer: no allocation.
  // Returns nullptr if the queue is full.
  T* producer_slot() noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);  // only we write it
    if (t - cached_head_ == slots_.size()) {
      cached_head_ = head_.load(kAcquire);  // pairs with consumer's release
      if (t - cached_head_ == slots_.size()) return nullptr;
    }
    return &slots_[t & mask_];
  }
  // Publishes the slot returned by producer_slot().
  void commit_push() noexcept {
    // Must follow a successful producer_slot() (debug builds check this).
    assert(tail_.load(std::memory_order_relaxed) - cached_head_ < slots_.size());
    tail_.store(tail_.load(std::memory_order_relaxed) + 1, kRelease);
  }

  template <class U>
  bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<T&, U&&>) {
    T* slot = producer_slot();
    if (!slot) return false;
    *slot = std::forward<U>(value);  // std::forward keeps rvalues as rvalues (moves)
    commit_push();
    return true;
  }

  // ---- Consumer side (one thread only) --------------------------------------

  // Returns the oldest element without removing it, or nullptr if empty.
  T* front() noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);  // only we write it
    if (h == cached_tail_) {
      cached_tail_ = tail_.load(kAcquire);  // pairs with producer's release
      if (h == cached_tail_) return nullptr;
    }
    return &slots_[h & mask_];
  }
  // Releases the slot returned by front() back to the producer.
  void pop() noexcept {
    // Must follow a successful front() (debug builds check this).
    assert(head_.load(std::memory_order_relaxed) != cached_tail_);
    head_.store(head_.load(std::memory_order_relaxed) + 1, kRelease);
  }

  bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
    T* slot = front();
    if (!slot) return false;
    out = std::move(*slot);
    pop();
    return true;
  }

  // Approximate when called while the other thread is active.
  std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  // Producer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
  // Read-only after construction (shared freely). Padding stops it sharing a
  // line with tail_.
  alignas(kCacheLine) std::vector<T> slots_;
  std::size_t mask_;
};

// Tells the CPU "I'm spinning": saves power and frees resources for the
// other hyper-thread, without giving up the core like a sleep would.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

}  // namespace pm
