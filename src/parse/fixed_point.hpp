#pragma once
// Fixed-point numbers: store decimals as whole numbers of a small unit.
//
// Price 0.08 is stored as 800 (units of 0.0001). Size 219.217767 is stored as
// 219217767 (units of 0.000001). Integers are exact, compare with ==, and can
// index an array directly. Floats can't do any of that reliably: 0.1 + 0.2 is
// 0.30000000000000004 in binary floating point. See learn/03.
//
// `constexpr` means the compiler is allowed to run these functions at compile
// time, which lets us check them with static_assert (a test that runs while
// compiling).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace pm {

using Price = std::int32_t;  // 1 unit = 0.0001
using Size = std::int64_t;   // 1 unit = 0.000001 shares

inline constexpr int kPriceDecimals = 4;
inline constexpr Price kPriceScale = 10'000;  // 1.0000
inline constexpr int kSizeDecimals = 6;
inline constexpr Size kSizeScale = 1'000'000;

// Parses a non-negative decimal string ("0.08", "33343.4", "1", ".5") into a
// count of 10^-Decimals units. Returns false (and leaves `out` alone) for
// anything that is not an exact, non-negative decimal: empty strings, signs,
// exponents, junk characters, overflow, or a non-zero digit past `Decimals`
// places (which would silently lose precision). Trailing zeros are fine.
template <int Decimals>
constexpr bool parse_fixed(std::string_view s, std::int64_t& out) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  std::int64_t value = 0;
  bool any_digit = false;
  std::size_t i = 0;

  // Appends one digit, refusing to overflow.
  auto push = [&value](int d) {
    if (value > (kMax - d) / 10) return false;
    value = value * 10 + d;
    return true;
  };

  for (; i < s.size() && s[i] != '.'; ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    if (!push(c - '0')) return false;
    any_digit = true;
  }

  int frac = 0;
  if (i < s.size()) {  // s[i] == '.'
    ++i;
    if (i == s.size()) return false;  // "5." is not accepted
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (c < '0' || c > '9') return false;
      any_digit = true;
      if (frac < Decimals) {
        if (!push(c - '0')) return false;
        ++frac;
      } else if (c != '0') {
        return false;  // more precision than we can represent exactly
      }
    }
  }
  if (!any_digit) return false;

  for (; frac < Decimals; ++frac) {
    if (!push(0)) return false;
  }
  out = value;
  return true;
}

// Price in [0, 1]. Polymarket prices are probabilities, so anything above 1.0
// is malformed.
constexpr bool parse_price(std::string_view s, Price& out) noexcept {
  std::int64_t v = 0;
  if (!parse_fixed<kPriceDecimals>(s, v) || v > kPriceScale) return false;
  out = static_cast<Price>(v);
  return true;
}

constexpr bool parse_size(std::string_view s, Size& out) noexcept {
  return parse_fixed<kSizeDecimals>(s, out);
}

// Plain non-negative integer, e.g. a millisecond timestamp "1793532756752".
constexpr bool parse_uint(std::string_view s, std::int64_t& out) noexcept {
  if (s.find('.') != std::string_view::npos) return false;
  return parse_fixed<0>(s, out);
}

// Writes `value` (in 10^-decimals units) as a short decimal string with
// trailing zeros removed: (800, 4) -> "0.08", (10000, 4) -> "1".
// Returns the number of chars written. `out` needs at least 24 bytes.
std::size_t format_fixed(std::int64_t value, int decimals, char* out) noexcept;
std::string format_fixed(std::int64_t value, int decimals);

inline std::string format_price(Price p) { return format_fixed(p, kPriceDecimals); }
inline std::string format_size(Size s) { return format_fixed(s, kSizeDecimals); }

// For output only (the PMBook protocol sends JSON numbers). Never feed these
// back into book logic.
constexpr double to_double(Price p) noexcept { return static_cast<double>(p) / kPriceScale; }
constexpr double size_to_double(Size s) noexcept {
  return static_cast<double>(s) / static_cast<double>(kSizeScale);
}

}  // namespace pm
