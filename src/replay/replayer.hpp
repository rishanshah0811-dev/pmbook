#pragma once
// Feeds recorded frames back through a callback, in recorded order.
//
//   speed = 0    as fast as possible (for tests and benchmarks)
//   speed = 1    real time: the same gaps between frames as when recorded
//   speed = 10   ten times faster than real time
//
// Timing uses the recorded steady-clock timestamps. Those only make sense
// within one recording session (steady clock restarts at reboot), so gaps are
// clamped to [0, max_gap]: a jump backwards or a long pause between two
// sessions in the same file won't stall or confuse the replay.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "record/record_reader.hpp"

namespace pm {

struct ReplayOptions {
  double speed = 0.0;
  std::chrono::milliseconds max_gap{60'000};
  const std::atomic<bool>* stop = nullptr;  // optional: set to true to stop early
};

struct ReplayResult {
  std::uint64_t records = 0;
  std::uint64_t files = 0;
  bool truncated = false;  // some file ended mid-stream (writer crashed or still writing)
  std::string error;       // non-empty on a hard error (unreadable file, malformed line)
  double wall_seconds = 0;
  double feed_seconds = 0;  // recorded time covered (sum of clamped gaps)
};

// Calls on_frame(std::string_view raw, std::int64_t recorded_ts_recv_ns) for
// every record. Files are replayed in file-name order, which for
// pm_YYYYMMDD_HH names is chronological order.
template <class F>
ReplayResult replay_files(std::vector<std::string> paths, const ReplayOptions& opt, F&& on_frame) {
  using clock = std::chrono::steady_clock;
  std::sort(paths.begin(), paths.end());
  ReplayResult res;
  RecordReader reader;
  Record rec;
  const auto wall_start = clock::now();
  std::int64_t prev_ts = 0;
  bool first = true;
  std::chrono::nanoseconds feed_elapsed{0};
  bool stopped = false;

  for (const auto& path : paths) {
    if (stopped) break;
    if (!reader.open(path)) {
      res.error = reader.error();
      break;
    }
    ++res.files;
    while (reader.next(rec)) {
      if (opt.stop && opt.stop->load(std::memory_order_relaxed)) {
        stopped = true;
        break;
      }
      if (!first) {
        auto gap = std::chrono::nanoseconds(std::max<std::int64_t>(0, rec.ts_recv_ns - prev_ts));
        feed_elapsed += std::min<std::chrono::nanoseconds>(gap, opt.max_gap);
      }
      first = false;
      prev_ts = rec.ts_recv_ns;
      if (opt.speed > 0) {
        const auto target =
            wall_start +
            std::chrono::duration_cast<clock::duration>(std::chrono::duration<double, std::nano>(
                static_cast<double>(feed_elapsed.count()) / opt.speed));
        std::this_thread::sleep_until(target);
      }
      on_frame(rec.raw, rec.ts_recv_ns);
      ++res.records;
    }
    res.truncated |= reader.truncated();
    if (!reader.error().empty()) {
      res.error = path + ": " + reader.error();
      break;
    }
  }
  res.wall_seconds = std::chrono::duration<double>(clock::now() - wall_start).count();
  res.feed_seconds = std::chrono::duration<double>(feed_elapsed).count();
  return res;
}

}  // namespace pm
