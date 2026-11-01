#pragma once
// network thread --(SpscQueue of frames)--> processing thread
//
// The network thread calls push(raw, recv_ns): it copies the frame into a
// pre-allocated slot (a std::string whose buffer is reused, so there's no
// allocation once warmed up) and returns immediately. The processing thread
// pops frames in order and calls the handler. Parsing and book updates never
// slow down the socket reads.
//
// Waiting when the queue is empty:
//   Spin    busy-loop with cpu_relax(). Lowest latency, but burns 100% of a
//           core even when the market is quiet.
//   Hybrid  spin ~50 us, then sleep in short naps. Nearly free when idle,
//           but the first frame after a quiet spell pays the OS wake-up
//           time (tens of microseconds or more). That cost shows up in our
//           latency numbers, honestly.
//
// Queue full: the producer waits (yields) until there's room. We never drop
// a frame, because a dropped delta would silently corrupt a book. The wait
// is counted in full_waits(), and since the receive timestamp is taken
// before the push, any queueing delay is included in measured latency.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "queue/spsc_queue.hpp"

namespace pm {

enum class WaitMode : std::uint8_t { Spin, Hybrid };

struct FrameSlot {
  std::int64_t recv_ns = 0;
  std::string data;
};

class FramePipeline {
 public:
  using Handler = std::function<void(std::string_view raw, std::int64_t recv_ns)>;

  FramePipeline(std::size_t capacity, WaitMode mode, Handler handler)
      : q_(capacity), mode_(mode), handler_(std::move(handler)) {}
  ~FramePipeline() { stop(); }
  FramePipeline(const FramePipeline&) = delete;
  FramePipeline& operator=(const FramePipeline&) = delete;

  void start() {
    running_.store(true, std::memory_order_release);
    consumer_ = std::thread([this] { run(); });
  }

  // Producer side. Call from ONE thread at a time (see FeedClient: frames of
  // consecutive connections never overlap).
  void push(std::string_view raw, std::int64_t recv_ns) {
    FrameSlot* slot;
    while ((slot = q_.producer_slot()) == nullptr) {
      full_waits_.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    }
    slot->recv_ns = recv_ns;
    slot->data.assign(raw.data(), raw.size());  // reuses the slot's buffer
    q_.commit_push();
  }

  // Processes everything already pushed, then stops the consumer thread.
  void stop() {
    if (!consumer_.joinable()) return;
    running_.store(false, std::memory_order_release);
    consumer_.join();
  }

  std::uint64_t full_waits() const noexcept { return full_waits_.load(std::memory_order_relaxed); }
  std::uint64_t processed() const noexcept { return processed_.load(std::memory_order_relaxed); }

 private:
  void run() {
    using namespace std::chrono;
    auto idle_since = steady_clock::now();
    bool idle = false;
    while (true) {
      if (FrameSlot* f = q_.front()) {
        handler_(f->data, f->recv_ns);
        q_.pop();
        processed_.fetch_add(1, std::memory_order_relaxed);
        idle = false;
        continue;
      }
      // Empty. Check for shutdown only now, so stop() drains the queue first.
      if (!running_.load(std::memory_order_acquire)) {
        if (q_.front() == nullptr) break;  // re-check: a push may have just landed
        continue;
      }
      if (mode_ == WaitMode::Spin) {
        cpu_relax();
        continue;
      }
      if (!idle) {
        idle = true;
        idle_since = steady_clock::now();
      }
      if (steady_clock::now() - idle_since < microseconds(50)) {
        cpu_relax();
      } else {
        std::this_thread::sleep_for(microseconds(50));
      }
    }
  }

  SpscQueue<FrameSlot> q_;
  WaitMode mode_;
  Handler handler_;
  std::thread consumer_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> full_waits_{0};
  std::atomic<std::uint64_t> processed_{0};
};

}  // namespace pm
