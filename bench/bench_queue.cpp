// SPSC lock-free queue vs mutex queue, two threads, both busy-spinning.
//
//   throughput : producer pushes N integers as fast as possible; items/s.
//   ping-pong  : thread A sends a number, thread B echoes it back on a second
//                queue; we time each round trip. One-way latency ~ RTT / 2.
//
// Writes results/m6_queue_bench.json. macOS doesn't allow pinning threads to
// cores, so the OS may move threads around; we take the median of 5 runs.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "queue/mutex_queue.hpp"
#include "queue/spsc_queue.hpp"

namespace {

using clk = std::chrono::steady_clock;

template <class Q>
double throughput_mops(std::uint64_t n) {
  Q q(1024);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < n;) {
      if (q.try_push(i)) {
        ++i;
      } else {
        pm::cpu_relax();
      }
    }
  });
  std::uint64_t got = 0, out = 0, sum = 0;
  const auto t0 = clk::now();
  while (got < n) {
    if (q.try_pop(out)) {
      sum += out;
      ++got;
    } else {
      pm::cpu_relax();
    }
  }
  const double secs = std::chrono::duration<double>(clk::now() - t0).count();
  producer.join();
  if (sum != n * (n - 1) / 2) std::fprintf(stderr, "checksum mismatch!\n");
  return static_cast<double>(n) / secs / 1e6;
}

struct Pct {
  double p50, p99, p999, max;
};

template <class Q>
Pct ping_pong_rtt_ns(std::size_t rounds) {
  Q to_b(64), to_a(64);
  std::thread b([&] {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < rounds; ++i) {
      while (!to_b.try_pop(v)) pm::cpu_relax();
      while (!to_a.try_push(v)) pm::cpu_relax();
    }
  });
  std::vector<double> rtt(rounds);
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < rounds; ++i) {
    const auto t0 = clk::now();
    while (!to_b.try_push(static_cast<std::uint64_t>(i))) pm::cpu_relax();
    while (!to_a.try_pop(v)) pm::cpu_relax();
    rtt[i] = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
  }
  b.join();
  std::sort(rtt.begin(), rtt.end());
  auto at = [&](double q) {
    return rtt[static_cast<std::size_t>(q * static_cast<double>(rounds - 1))];
  };
  return {at(0.50), at(0.99), at(0.999), rtt.back()};
}

// Smallest non-zero step of steady_clock: the resolution of every timing here.
double clock_resolution_ns() {
  double best = 1e9;
  for (int i = 0; i < 100000; ++i) {
    const auto a = clk::now();
    auto b = clk::now();
    while (b == a) b = clk::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count());
  }
  return best;
}

template <class T>
T median_of(std::vector<T> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double median_by(std::vector<Pct> v, double Pct::* f) {
  std::vector<double> x;
  for (auto& p : v) x.push_back(p.*f);
  return median_of(x);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_path = argc > 1 ? argv[1] : "results/m6_queue_bench.json";
  constexpr std::uint64_t kItems = 50'000'000;
  constexpr std::size_t kRounds = 1'000'000;
  constexpr int kRuns = 5;

  std::vector<double> spsc_tp, mutex_tp;
  std::vector<Pct> spsc_rtt, mutex_rtt;
  for (int r = 0; r < kRuns; ++r) {
    spsc_tp.push_back(throughput_mops<pm::SpscQueue<std::uint64_t>>(kItems));
    mutex_tp.push_back(throughput_mops<pm::MutexQueue<std::uint64_t>>(kItems / 10));
    spsc_rtt.push_back(ping_pong_rtt_ns<pm::SpscQueue<std::uint64_t>>(kRounds));
    mutex_rtt.push_back(ping_pong_rtt_ns<pm::MutexQueue<std::uint64_t>>(kRounds));
    std::fprintf(stderr,
                 "run %d: spsc %.1f M/s, mutex %.1f M/s, rtt p50 spsc %.0f ns, mutex %.0f ns\n",
                 r + 1, spsc_tp.back(), mutex_tp.back(), spsc_rtt.back().p50, mutex_rtt.back().p50);
  }

  char json[2048];
  std::snprintf(json, sizeof json,
                "{\n  \"machine\": \"Apple M5, 16 GB, macOS 26.6.2\",\n"
                "  \"compiler\": \"Homebrew clang 23.1.4, -O3, LTO\",\n"
                "  \"method\": \"2 threads, both busy-spinning; median of %d runs; no core pinning "
                "(macOS)\",\n"
                "  \"clock_resolution_ns\": %.2f,\n"
                "  \"throughput_million_items_per_s\": {\"spsc\": %.1f, \"mutex\": %.1f},\n"
                "  \"ping_pong_round_trip_ns\": {\n"
                "    \"spsc\":  {\"p50\": %.0f, \"p99\": %.0f, \"p99_9\": %.0f},\n"
                "    \"mutex\": {\"p50\": %.0f, \"p99\": %.0f, \"p99_9\": %.0f}\n  }\n}\n",
                kRuns, clock_resolution_ns(), median_of(spsc_tp), median_of(mutex_tp),
                median_by(spsc_rtt, &Pct::p50), median_by(spsc_rtt, &Pct::p99),
                median_by(spsc_rtt, &Pct::p999), median_by(mutex_rtt, &Pct::p50),
                median_by(mutex_rtt, &Pct::p99), median_by(mutex_rtt, &Pct::p999));
  std::ofstream(out_path) << json;
  std::fputs(json, stdout);
  return 0;
}
