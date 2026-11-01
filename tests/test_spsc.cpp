#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "queue/frame_pipeline.hpp"
#include "queue/mutex_queue.hpp"
#include "queue/spsc_queue.hpp"

using namespace pm;

namespace {
// Sanitizers slow code down 5-20x, so stress fewer items under them.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::uint64_t kStressItems = 1'000'000;
#else
constexpr std::uint64_t kStressItems = 20'000'000;
#endif
#else
constexpr std::uint64_t kStressItems = 20'000'000;
#endif
}  // namespace

TEST(Spsc, RejectsNonPowerOfTwoCapacity) {
  EXPECT_THROW(SpscQueue<int>(3), std::invalid_argument);
  EXPECT_THROW(SpscQueue<int>(1), std::invalid_argument);
  EXPECT_NO_THROW(SpscQueue<int>(2));
}

TEST(Spsc, EmptyFullAndFifoOrder) {
  SpscQueue<int> q(4);
  int out = -1;
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_EQ(q.front(), nullptr);
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(99));  // full
  EXPECT_EQ(q.size_approx(), 4u);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(q.try_pop(out));
}

TEST(Spsc, WrapsAroundManyTimes) {
  SpscQueue<std::uint64_t> q(8);
  std::uint64_t next_in = 0, next_out = 0, out = 0;
  for (int round = 0; round < 10'000; ++round) {
    const int n = round % 8 + 1;  // vary fill level
    for (int i = 0; i < n; ++i) ASSERT_TRUE(q.try_push(next_in++));
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(q.try_pop(out));
      ASSERT_EQ(out, next_out++);
    }
  }
}

TEST(Spsc, TwoPhaseApiFillsInPlace) {
  SpscQueue<std::string> q(2);
  std::string* s = q.producer_slot();
  ASSERT_NE(s, nullptr);
  s->assign("hello");
  EXPECT_EQ(q.front(), nullptr);  // not visible until committed
  q.commit_push();
  ASSERT_NE(q.front(), nullptr);
  EXPECT_EQ(*q.front(), "hello");
  q.pop();
  EXPECT_EQ(q.front(), nullptr);
}

#ifndef NDEBUG
TEST(SpscDeathTest, PopWithoutFrontIsCaughtInDebugBuilds) {
  // Misuse: pop() must only follow a successful front().
  EXPECT_DEATH(
      {
        SpscQueue<int> q(2);
        q.pop();
      },
      "");
}
#endif

TEST(Spsc, MoveOnlyTypes) {
  SpscQueue<std::unique_ptr<int>> q(2);
  EXPECT_TRUE(q.try_push(std::make_unique<int>(7)));
  std::unique_ptr<int> out;
  ASSERT_TRUE(q.try_pop(out));
  EXPECT_EQ(*out, 7);
}

// Two threads, tiny queue (constantly flipping between full and empty).
// The consumer checks every value arrives exactly once, in order.
template <class Q>
void stress_sequence(std::size_t capacity, std::uint64_t items) {
  Q q(capacity);
  std::atomic<bool> abort{false};  // lets the producer quit if we fail early
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < items && !abort.load(std::memory_order_relaxed);) {
      if (q.try_push(i)) ++i;
    }
  });
  std::uint64_t expected = 0;
  std::uint64_t out = 0;
  bool ok = true;
  while (expected < items) {
    if (q.try_pop(out)) {
      if (out != expected) {
        ok = false;
        abort.store(true, std::memory_order_relaxed);
        break;
      }
      ++expected;
    }
  }
  producer.join();
  ASSERT_TRUE(ok) << "got " << out << " expected " << expected;
  EXPECT_EQ(expected, items);
}

TEST(SpscStress, SequenceCapacity2) { stress_sequence<SpscQueue<std::uint64_t>>(2, kStressItems); }
TEST(SpscStress, SequenceCapacity16) {
  stress_sequence<SpscQueue<std::uint64_t>>(16, kStressItems);
}
TEST(SpscStress, SequenceCapacity1024) {
  stress_sequence<SpscQueue<std::uint64_t>>(1024, kStressItems);
}
TEST(SpscStress, MutexQueueBaselineIsAlsoCorrect) {
  stress_sequence<MutexQueue<std::uint64_t>>(16, kStressItems / 10);
}

// Payloads written in place through the two-phase API: if the consumer ever
// saw a half-written slot, the content wouldn't match its sequence number.
// Payloads are prepared up front so neither loop allocates memory: extra
// allocator activity in both threads can hide races from ThreadSanitizer.
TEST(SpscStress, InPlaceStringPayloadsAreNeverTorn) {
  constexpr std::size_t kDistinct = 4096;
  const std::uint64_t n = kStressItems / 10;
  std::vector<std::string> payloads(kDistinct);
  for (std::size_t i = 0; i < kDistinct; ++i) {
    payloads[i] = std::string(i % 200 + 1, static_cast<char>('a' + i % 26)) + std::to_string(i);
  }
  SpscQueue<std::string> q(8);
  for (std::size_t i = 0; i < q.capacity(); ++i) {  // pre-size slot buffers
    std::string* s = q.producer_slot();
    s->reserve(256);
    q.commit_push();
    ASSERT_NE(q.front(), nullptr);  // protocol: front() before pop()
    q.pop();
  }
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < n; ++i) {
      std::string* s;
      while ((s = q.producer_slot()) == nullptr) cpu_relax();
      s->assign(payloads[i % kDistinct]);  // fits in reserved buffer: no allocation
      q.commit_push();
    }
  });
  bool ok = true;
  std::uint64_t i = 0;
  for (; i < n && ok; ++i) {
    std::string* s;
    while ((s = q.front()) == nullptr) cpu_relax();
    ok = (*s == payloads[i % kDistinct]);
    q.pop();
  }
  // On failure keep draining so the producer can finish and join.
  for (; i < n; ++i) {
    while (q.front() == nullptr) cpu_relax();
    q.pop();
  }
  producer.join();
  EXPECT_TRUE(ok);
}

// ---- FramePipeline ----------------------------------------------------------

namespace {
void pipeline_delivers_all_in_order(WaitMode mode) {
  std::vector<std::pair<std::string, std::int64_t>> got;  // only the consumer thread writes
  FramePipeline p(4, mode,
                  [&](std::string_view raw, std::int64_t ns) { got.emplace_back(raw, ns); });
  p.start();
  constexpr int kN = 20'000;
  for (int i = 0; i < kN; ++i) p.push("frame " + std::to_string(i), i);
  p.stop();  // drains, then joins: safe to read `got` after this
  ASSERT_EQ(got.size(), static_cast<std::size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    ASSERT_EQ(got[static_cast<std::size_t>(i)].first, "frame " + std::to_string(i));
    ASSERT_EQ(got[static_cast<std::size_t>(i)].second, i);
  }
  EXPECT_EQ(p.processed(), static_cast<std::uint64_t>(kN));
  EXPECT_GT(p.full_waits(), 0u);  // capacity 4: the producer must have waited
}
}  // namespace

TEST(FramePipeline, SpinModeDeliversEverythingInOrder) {
  pipeline_delivers_all_in_order(WaitMode::Spin);
}
TEST(FramePipeline, HybridModeDeliversEverythingInOrder) {
  pipeline_delivers_all_in_order(WaitMode::Hybrid);
}

TEST(FramePipeline, HybridWakesUpAfterIdle) {
  std::atomic<int> n{0};
  FramePipeline p(8, WaitMode::Hybrid, [&](std::string_view, std::int64_t) { n.fetch_add(1); });
  p.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // consumer is now napping
  p.push("late", 1);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (n.load() == 0 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
  EXPECT_EQ(n.load(), 1);
  p.stop();
}

TEST(FramePipeline, StopWithoutStartIsSafe) {
  FramePipeline p(2, WaitMode::Spin, [](std::string_view, std::int64_t) {});
  p.stop();
}
