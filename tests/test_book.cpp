#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <vector>

#include "book/book_math.hpp"
#include "book/flat_book.hpp"
#include "book/map_book.hpp"

using namespace pm;

// ---- Tests that run against BOTH book implementations ----------------------
// TYPED_TEST runs each test once per type in BookTypes.

template <class B>
struct BookTest : ::testing::Test {
  std::unique_ptr<B> book = std::make_unique<B>();
  B& b() { return *book; }
};
using BookTypes = ::testing::Types<FlatBook, MapBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

namespace {
std::vector<Level> top(const auto& b, Side s, std::size_t n) {
  std::vector<Level> out(n);
  out.resize(b.top_levels(s, out));
  return out;
}
}  // namespace

TYPED_TEST(BookTest, EmptyBook) {
  auto& b = this->b();
  EXPECT_EQ(b.best_bid(), kNoPrice);
  EXPECT_EQ(b.best_ask(), kNoPrice);
  EXPECT_EQ(b.level_count(Side::Buy), 0u);
  EXPECT_FALSE(is_crossed(b));
  EXPECT_FALSE(microprice(b).has_value());
  EXPECT_TRUE(top(b, Side::Buy, 5).empty());
}

TYPED_TEST(BookTest, SnapshotInAnyOrderComesOutSorted) {
  auto& b = this->b();
  // Same order as the docs example: bids ascending, asks descending.
  const std::vector<Level> bids{{800, 5}, {900, 7}, {100, 1}};
  const std::vector<Level> asks{{9900, 3}, {9800, 4}};
  ASSERT_TRUE(b.apply_snapshot(bids, asks));
  EXPECT_EQ(b.best_bid(), 900);
  EXPECT_EQ(b.best_ask(), 9800);
  EXPECT_EQ(top(b, Side::Buy, 10), (std::vector<Level>{{900, 7}, {800, 5}, {100, 1}}));
  EXPECT_EQ(top(b, Side::Sell, 10), (std::vector<Level>{{9800, 4}, {9900, 3}}));
  EXPECT_EQ(top(b, Side::Buy, 2).size(), 2u);  // truncation
}

TYPED_TEST(BookTest, SnapshotReplacesEverything) {
  auto& b = this->b();
  std::vector<Level> bids{{5000, 1}, {4000, 1}};
  b.apply_snapshot(bids, {});
  std::vector<Level> bids2{{3000, 2}};
  b.apply_snapshot(bids2, {});
  EXPECT_EQ(b.level_count(Side::Buy), 1u);
  EXPECT_EQ(b.best_bid(), 3000);
  EXPECT_EQ(b.size_at(Side::Buy, 5000), 0);
}

TYPED_TEST(BookTest, SnapshotSkipsZeroSizeLevels) {
  auto& b = this->b();
  std::vector<Level> bids{{5000, 0}, {4000, 3}};
  b.apply_snapshot(bids, {});
  EXPECT_EQ(b.level_count(Side::Buy), 1u);
  EXPECT_EQ(b.best_bid(), 4000);
}

TYPED_TEST(BookTest, DeltasAddUpdateAndRemove) {
  auto& b = this->b();
  b.set_level(Side::Buy, 6100, 500);
  b.set_level(Side::Buy, 6000, 1200);
  b.set_level(Side::Sell, 6300, 300);
  b.set_level(Side::Sell, 6400, 900);
  EXPECT_EQ(b.best_bid(), 6100);
  EXPECT_EQ(b.best_ask(), 6300);

  b.set_level(Side::Buy, 6200, 400);  // new best bid (lesson 01's example)
  EXPECT_EQ(b.best_bid(), 6200);

  b.set_level(Side::Buy, 6200, 450);  // size update, same level
  EXPECT_EQ(b.level_count(Side::Buy), 3u);
  EXPECT_EQ(b.size_at(Side::Buy, 6200), 450);

  b.set_level(Side::Sell, 6300, 0);  // remove best ask -> next level
  EXPECT_EQ(b.best_ask(), 6400);
  EXPECT_EQ(b.level_count(Side::Sell), 1u);

  b.set_level(Side::Buy, 6000, 0);  // remove a non-best level
  EXPECT_EQ(b.best_bid(), 6200);
  EXPECT_EQ(b.level_count(Side::Buy), 2u);

  b.set_level(Side::Buy, 5000, 0);  // removing a level that isn't there: no-op
  EXPECT_EQ(b.level_count(Side::Buy), 2u);

  b.set_level(Side::Sell, 6400, 0);  // side becomes empty
  EXPECT_EQ(b.best_ask(), kNoPrice);
}

TYPED_TEST(BookTest, BitmapWordBoundariesAndExtremes) {
  auto& b = this->b();
  // 63/64 and 127/128 straddle 64-bit word edges; 0 and 10000 are the ends.
  for (Price p : {0, 63, 64, 127, 128, 9999, 10000}) b.set_level(Side::Buy, p, 1);
  for (Price p : {0, 63, 64, 127, 128, 9999, 10000}) b.set_level(Side::Sell, p, 1);
  std::vector<Price> seen_bids;
  for (Price expect : {10000, 9999, 128, 127, 64, 63, 0}) {
    EXPECT_EQ(b.best_bid(), expect);
    seen_bids.push_back(b.best_bid());
    b.set_level(Side::Buy, expect, 0);
  }
  EXPECT_EQ(b.best_bid(), kNoPrice);
  for (Price expect : {0, 63, 64, 127, 128, 9999, 10000}) {
    EXPECT_EQ(b.best_ask(), expect);
    b.set_level(Side::Sell, expect, 0);
  }
  EXPECT_EQ(b.best_ask(), kNoPrice);
}

TYPED_TEST(BookTest, RejectsOutOfRange) {
  auto& b = this->b();
  EXPECT_FALSE(b.set_level(Side::Buy, -1, 1));
  EXPECT_FALSE(b.set_level(Side::Buy, 10001, 1));
  EXPECT_FALSE(b.set_level(Side::Buy, 5000, -1));
  EXPECT_EQ(b.level_count(Side::Buy), 0u);
}

TYPED_TEST(BookTest, CrossedDetection) {
  auto& b = this->b();
  b.set_level(Side::Buy, 5000, 1);
  b.set_level(Side::Sell, 5100, 1);
  EXPECT_FALSE(is_crossed(b));
  b.set_level(Side::Buy, 5100, 1);  // bid == ask ("locked") counts as crossed
  EXPECT_TRUE(is_crossed(b));
  b.set_level(Side::Buy, 5100, 0);
  b.set_level(Side::Buy, 5200, 1);  // bid > ask
  EXPECT_TRUE(is_crossed(b));
}

TYPED_TEST(BookTest, DepthAndMicroprice) {
  auto& b = this->b();
  std::vector<Level> bids{{6100, 500}, {6000, 1200}};
  std::vector<Level> asks{{6300, 300}, {6400, 900}};
  b.apply_snapshot(bids, asks);
  EXPECT_EQ(depth(b, Side::Buy, 1), 500);
  EXPECT_EQ(depth(b, Side::Buy, 10), 1700);
  EXPECT_EQ(depth(b, Side::Sell, 2), 1200);
  // (0.61*300 + 0.63*500) / 800 = 0.6225: more size on the bid pulls it up.
  ASSERT_TRUE(microprice(b).has_value());
  EXPECT_NEAR(*microprice(b), 0.6225, 1e-12);
}

// ---- The big one: flat book vs std::map reference on 1M random updates -----

TEST(FlatVsMap, OneMillionRandomUpdatesAlwaysIdentical) {
  auto flat = std::make_unique<FlatBook>();
  MapBook ref;
  std::mt19937_64 rng(20261101);
  std::uniform_int_distribution<int> pct(0, 99);
  std::uniform_int_distribution<Size> size_dist(1, 1'000'000'000'000);
  int mid = 5000;

  auto check_top = [&] {
    ASSERT_EQ(flat->best_bid(), ref.best_bid());
    ASSERT_EQ(flat->best_ask(), ref.best_ask());
    ASSERT_EQ(flat->level_count(Side::Buy), ref.level_count(Side::Buy));
    ASSERT_EQ(flat->level_count(Side::Sell), ref.level_count(Side::Sell));
  };

  constexpr int kUpdates = 1'000'000;
  for (int i = 0; i < kUpdates; ++i) {
    // Random walk of the "mid" so activity moves around the whole range.
    mid = std::clamp(mid + std::uniform_int_distribution<int>(-3, 3)(rng), 50, 9950);

    if (i % 100'000 == 0) {  // occasional full snapshot
      std::vector<Level> bids, asks;
      for (int k = 0; k < 40; ++k) {
        bids.push_back({std::clamp(mid - 1 - pct(rng), 0, 10000), size_dist(rng)});
        asks.push_back({std::clamp(mid + 1 + pct(rng), 0, 10000), size_dist(rng)});
      }
      flat->apply_snapshot(bids, asks);
      ref.apply_snapshot(bids, asks);
    } else {
      const Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
      Price p;
      const int r = pct(rng);
      if (r < 85) {  // most activity near the top of book
        const int off = std::uniform_int_distribution<int>(0, 60)(rng);
        p = side == Side::Buy ? mid - off : mid + off;
      } else if (r < 97) {
        p = std::uniform_int_distribution<Price>(0, kPriceScale)(rng);  // anywhere
      } else {
        p = pct(rng) < 50 ? 0 : kPriceScale;  // extremes
      }
      p = std::clamp(p, 0, kPriceScale);
      const Size s = pct(rng) < 35 ? 0 : size_dist(rng);  // 35% removals
      ASSERT_EQ(flat->set_level(side, p, s), ref.set_level(side, p, s));
      ASSERT_EQ(flat->size_at(side, p), ref.size_at(side, p));
    }
    check_top();
    if (::testing::Test::HasFatalFailure()) {
      FAIL() << "diverged at update " << i;
    }
    if (i % 1000 == 0) {
      ASSERT_EQ(state_hash(*flat), state_hash(ref)) << "at update " << i;
      ASSERT_EQ(top(*flat, Side::Buy, 10), top(ref, Side::Buy, 10)) << "at update " << i;
      ASSERT_EQ(top(*flat, Side::Sell, 10), top(ref, Side::Sell, 10)) << "at update " << i;
    }
  }
  EXPECT_EQ(state_hash(*flat), state_hash(ref));
  EXPECT_GT(flat->level_count(Side::Buy) + flat->level_count(Side::Sell), 0u);
}
