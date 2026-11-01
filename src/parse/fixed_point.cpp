#include "parse/fixed_point.hpp"

namespace pm {

std::size_t format_fixed(std::int64_t value, int decimals, char* out) noexcept {
  char tmp[24];
  std::size_t n = 0;
  const bool negative = value < 0;
  // Work in unsigned so that INT64_MIN does not overflow when negated.
  std::uint64_t v =
      negative ? (~static_cast<std::uint64_t>(value) + 1) : static_cast<std::uint64_t>(value);

  // Emit digits right to left, inserting the decimal point after `decimals`.
  int written_frac = 0;
  bool seen_nonzero_frac = false;
  for (; written_frac < decimals; ++written_frac) {
    const char d = static_cast<char>('0' + v % 10);
    v /= 10;
    if (d != '0' || seen_nonzero_frac) {
      tmp[n++] = d;
      seen_nonzero_frac = true;
    }
  }
  if (seen_nonzero_frac) tmp[n++] = '.';
  do {
    tmp[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  if (negative) tmp[n++] = '-';

  for (std::size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}

std::string format_fixed(std::int64_t value, int decimals) {
  char buf[24];
  return std::string(buf, format_fixed(value, decimals, buf));
}

}  // namespace pm
