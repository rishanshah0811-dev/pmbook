#pragma once
// Latency histogram with log-scaled buckets (the idea behind HdrHistogram).
//
// Recording a value is O(1) and never allocates, so it's safe on the hot path.
// Values are grouped into buckets: each power of two [2^k, 2^(k+1)) is split
// into 64 equal sub-buckets. So every bucket is at most 1/64 (~1.6%) of its
// value wide: 1,000 ns lands in a 16 ns-wide bucket, 1,000,000 ns in a
// 16,384 ns one. That's precise where it matters and tiny in memory
// (~2,700 counters cover 1 ns to ~18 minutes).
//
// Percentiles are reported as the upper edge of the bucket holding that rank,
// so they never under-report latency. Min, max and mean are exact.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace pm {

class LatencyHistogram {
 public:
  static constexpr int kSubBits = 6;  // 64 sub-buckets per power of two
  static constexpr int kSub = 1 << kSubBits;
  static constexpr int kMaxExp = 40;  // values up to 2^40 ns (~18 min)
  static constexpr int kBuckets = (kMaxExp - kSubBits + 1) * kSub + kSub;

  void record(std::int64_t ns) noexcept {
    const std::uint64_t v = ns < 0 ? 0 : static_cast<std::uint64_t>(ns);
    ++counts_[static_cast<std::size_t>(index_of(v))];
    ++count_;
    sum_ += v;
    if (v < min_) min_ = v;
    if (v > max_) max_ = v;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  std::uint64_t max() const noexcept { return max_; }
  double mean() const noexcept {
    return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
  }

  // q in [0, 1], e.g. 0.99 for p99. Returns an upper bound for that percentile
  // (exact max for q = 1).
  std::uint64_t percentile(double q) const noexcept {
    if (count_ == 0) return 0;
    if (q >= 1.0) return max_;
    // Rank of the q-th value, 1-based: the smallest value with at least
    // ceil(q * count) values at or below it.
    auto rank = static_cast<std::uint64_t>(q * static_cast<double>(count_));
    if (static_cast<double>(rank) < q * static_cast<double>(count_)) ++rank;
    if (rank == 0) rank = 1;
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts_[static_cast<std::size_t>(i)];
      if (seen >= rank) {
        const std::uint64_t hi = upper_edge(i);
        return hi < max_ ? hi : max_;
      }
    }
    return max_;
  }

  void merge(const LatencyHistogram& o) noexcept {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
    count_ += o.count_;
    sum_ += o.sum_;
    if (o.count_ && o.min_ < min_) min_ = o.min_;
    if (o.max_ > max_) max_ = o.max_;
  }

  // JSON object with count, min, mean, percentiles, max and the non-empty
  // buckets (for charts). All values in nanoseconds.
  std::string to_json() const {
    std::string out = "{\"count\":" + std::to_string(count_) +
                      ",\"min_ns\":" + std::to_string(min()) + ",\"mean_ns\":" + fmt(mean()) +
                      ",\"p50_ns\":" + std::to_string(percentile(0.50)) +
                      ",\"p90_ns\":" + std::to_string(percentile(0.90)) +
                      ",\"p99_ns\":" + std::to_string(percentile(0.99)) +
                      ",\"p99_9_ns\":" + std::to_string(percentile(0.999)) +
                      ",\"max_ns\":" + std::to_string(max_) + ",\"buckets\":[";
    bool first = true;
    for (int i = 0; i < kBuckets; ++i) {
      const auto c = counts_[static_cast<std::size_t>(i)];
      if (!c) continue;
      if (!first) out += ',';
      first = false;
      out += "[" + std::to_string(lower_edge(i)) + "," + std::to_string(upper_edge(i)) + "," +
             std::to_string(c) + "]";
    }
    out += "]}";
    return out;
  }

  // Bucket index for value v. Public for tests.
  static int index_of(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<int>(v);  // 0..63 exact
    const int exp = 63 - std::countl_zero(v);  // v in [2^exp, 2^(exp+1))
    if (exp > kMaxExp) return kBuckets - 1;
    const int shift = exp - kSubBits;
    const int sub = static_cast<int>((v >> shift) & (kSub - 1));
    return (exp - kSubBits + 1) * kSub + sub;
  }
  static std::uint64_t lower_edge(int i) noexcept {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const int exp = i / kSub + kSubBits - 1;
    const int sub = i % kSub;
    return (std::uint64_t{1} << exp) + (static_cast<std::uint64_t>(sub) << (exp - kSubBits));
  }
  static std::uint64_t upper_edge(int i) noexcept {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const int exp = i / kSub + kSubBits - 1;
    return lower_edge(i) + (std::uint64_t{1} << (exp - kSubBits)) - 1;
  }

 private:
  static std::string fmt(double d) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", d);
    return b;
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace pm
