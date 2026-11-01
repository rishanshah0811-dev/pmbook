#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>

#include "parse/fixed_point.hpp"

using namespace pm;

// constexpr functions can be tested by the compiler itself.
static_assert([] {
  Price p = 0;
  return parse_price("0.08", p) && p == 800;
}());
static_assert([] {
  Price p = 0;
  return !parse_price("1.0001", p);
}());

namespace {

std::int64_t price_or(std::string_view s, std::int64_t fallback = -1) {
  Price p = 0;
  return parse_price(s, p) ? p : fallback;
}
std::int64_t size_or(std::string_view s, std::int64_t fallback = -1) {
  Size v = 0;
  return parse_size(s, v) ? v : fallback;
}

}  // namespace

TEST(FixedPoint, ParsesPrices) {
  EXPECT_EQ(price_or("0.08"), 800);
  EXPECT_EQ(price_or("0.99"), 9900);
  EXPECT_EQ(price_or("0.001"), 10);      // a 0.001-tick market
  EXPECT_EQ(price_or("0.0001"), 1);      // smallest representable price
  EXPECT_EQ(price_or("0.250"), 2500);    // seen live as last_trade_price
  EXPECT_EQ(price_or("0.25000"), 2500);  // trailing zeros are exact, so allowed
  EXPECT_EQ(price_or("1"), 10000);
  EXPECT_EQ(price_or("1.0"), 10000);
  EXPECT_EQ(price_or("0"), 0);
  EXPECT_EQ(price_or(".5"), 5000);
}

TEST(FixedPoint, RejectsBadPrices) {
  for (const char* bad : {"", ".", "5.", "-0.1", "+0.1", "1.0001", "2", "0.00005", "1e-2", "0.1.2",
                          "abc", " 0.1", "0.1 ", "0,1", "NaN"}) {
    EXPECT_EQ(price_or(bad), -1) << "should reject: '" << bad << "'";
  }
}

TEST(FixedPoint, ParsesSizes) {
  EXPECT_EQ(size_or("33343.4"), 33'343'400'000);
  EXPECT_EQ(size_or("219.217767"), 219'217'767);
  EXPECT_EQ(size_or("0"), 0);  // size "0" in a delta = remove the level
  EXPECT_EQ(size_or("11010"), 11'010'000'000);
  EXPECT_EQ(size_or("1660432.06"), 1'660'432'060'000);  // largest size seen live
}

TEST(FixedPoint, RejectsBadSizes) {
  EXPECT_EQ(size_or("0.0000001"), -1);                    // 7th decimal would be lost
  EXPECT_EQ(size_or("9223372036854.775808"), -1);         // INT64_MAX + 1 micro-units
  EXPECT_EQ(size_or("99999999999999999999"), -1);         // overflow
  EXPECT_EQ(size_or("9223372036854.775807"), INT64_MAX);  // exactly the max still fits
}

TEST(FixedPoint, ParsesTimestamps) {
  std::int64_t ts = 0;
  ASSERT_TRUE(parse_uint("1793532756752", ts));
  EXPECT_EQ(ts, 1'793'532'756'752);
  EXPECT_FALSE(parse_uint("1.5", ts));
  EXPECT_FALSE(parse_uint("", ts));
}

TEST(FixedPoint, FormatsWithoutTrailingZeros) {
  EXPECT_EQ(format_price(800), "0.08");
  EXPECT_EQ(format_price(10000), "1");
  EXPECT_EQ(format_price(0), "0");
  EXPECT_EQ(format_price(1), "0.0001");
  EXPECT_EQ(format_size(33'343'400'000), "33343.4");
  EXPECT_EQ(format_size(219'217'767), "219.217767");
  EXPECT_EQ(format_fixed(-1500, 4), "-0.15");
  EXPECT_EQ(format_fixed(INT64_MIN, 0), "-9223372036854775808");
}

TEST(FixedPoint, FormatThenParseRoundTrips) {
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<std::int64_t> sizes(0, INT64_MAX);
  std::uniform_int_distribution<Price> prices(0, kPriceScale);
  for (int i = 0; i < 100'000; ++i) {
    const Size s = sizes(rng);
    Size back = -1;
    ASSERT_TRUE(parse_size(format_size(s), back));
    ASSERT_EQ(back, s);

    const Price p = prices(rng);
    Price pback = -1;
    ASSERT_TRUE(parse_price(format_price(p), pback));
    ASSERT_EQ(pback, p);
  }
}

TEST(FixedPoint, WhyNotFloats) {
  // The classic: binary floating point can't represent 0.1 exactly.
  EXPECT_NE(0.1 + 0.2, 0.3);
  // Fixed point has no such problem.
  Price a = 0, b = 0, c = 0;
  ASSERT_TRUE(parse_price("0.1", a) && parse_price("0.2", b) && parse_price("0.3", c));
  EXPECT_EQ(a + b, c);
}
