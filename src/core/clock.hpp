#pragma once
// Two clocks, two jobs:
//  * steady_clock never jumps (not even when the OS adjusts the time), so it
//    is the right clock for measuring durations and latency.
//  * system_clock is wall time (what a calendar shows). We only use it to
//    name files by date/hour.

#include <chrono>
#include <cstdint>

namespace pm {

inline std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::int64_t wall_seconds() noexcept {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace pm
