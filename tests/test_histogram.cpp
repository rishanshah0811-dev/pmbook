#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

#include "core/histogram.hpp"

using pm::LatencyHistogram;

TEST(Histogram, EmptyIsZero) {
  LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.percentile(0.5), 0u);
  EXPECT_EQ(h.max(), 0u);
}

TEST(Histogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (int v = 0; v < 64; ++v) h.record(v);
  EXPECT_EQ(h.percentile(0.5), 31u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.max(), 63u);
}

TEST(Histogram, BucketsTileTheRangeWithoutGaps) {
  for (int i = 1; i < LatencyHistogram::kBuckets - 1; ++i) {
    ASSERT_EQ(LatencyHistogram::lower_edge(i), LatencyHistogram::upper_edge(i - 1) + 1) << i;
  }
  for (std::uint64_t v : {0ull, 63ull, 64ull, 65ull, 1000ull, 1'000'000ull, 123'456'789ull}) {
    const int i = LatencyHistogram::index_of(v);
    EXPECT_LE(LatencyHistogram::lower_edge(i), v);
    EXPECT_GE(LatencyHistogram::upper_edge(i), v);
  }
}

// Percentiles must match exact sorted-array percentiles within the bucket
// width (1/64 relative), and never under-report.
TEST(Histogram, PercentilesMatchExactWithinOnePointSixPercent) {
  std::mt19937_64 rng(1);
  std::lognormal_distribution<double> dist(9.0, 1.5);  // heavy tail, median ~8 us
  LatencyHistogram h;
  std::vector<std::uint64_t> v;
  for (int i = 0; i < 200'000; ++i) {
    const auto x = static_cast<std::uint64_t>(dist(rng));
    v.push_back(x);
    h.record(static_cast<std::int64_t>(x));
  }
  std::sort(v.begin(), v.end());
  for (double q : {0.5, 0.9, 0.99, 0.999}) {
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(v.size())));
    const std::uint64_t exact = v[rank - 1];
    const std::uint64_t got = h.percentile(q);
    EXPECT_GE(got, exact) << "q=" << q;
    EXPECT_LE(static_cast<double>(got), static_cast<double>(exact) * (1.0 + 1.0 / 64) + 1)
        << "q=" << q;
  }
  EXPECT_EQ(h.percentile(1.0), v.back());
  EXPECT_EQ(h.min(), v.front());
}

TEST(Histogram, MergeEqualsRecordingEverything) {
  LatencyHistogram a, b, all;
  for (int i = 1; i < 5000; ++i) {
    (i % 2 ? a : b).record(i * 37);
    all.record(i * 37);
  }
  a.merge(b);
  EXPECT_EQ(a.to_json(), all.to_json());
}
