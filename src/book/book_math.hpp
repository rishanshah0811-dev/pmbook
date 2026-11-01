#pragma once
// Read-only calculations that work on any book type with the FlatBook /
// MapBook interface. Written as templates so both books share one
// implementation (and one set of tests).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "book/flat_book.hpp"

namespace pm {

// Fingerprint of the full book: FNV-1a 64-bit over every (side, price, size),
// best first. Equal books give equal hashes, so we can compare two books
// (or a live run vs a replay) by one number.
template <class Book>
std::uint64_t state_hash(const Book& b) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV offset basis
  auto mix = [&h](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 1099511628211ULL;  // FNV prime
    }
  };
  for (Side s : {Side::Buy, Side::Sell}) {
    mix(s == Side::Buy ? 0xB1D : 0xA5C);
    b.for_each_level(s, [&](Level l) {
      mix(static_cast<std::uint64_t>(l.price));
      mix(static_cast<std::uint64_t>(l.size));
    });
  }
  return h;
}

// Best bid >= best ask. On a real exchange those would have traded, so in a
// local copy it means we missed or misapplied an update.
template <class Book>
bool is_crossed(const Book& b) noexcept {
  return b.best_bid() != kNoPrice && b.best_ask() != kNoPrice && b.best_bid() >= b.best_ask();
}

// Total size in the best `levels` levels on one side.
template <class Book>
Size depth(const Book& b, Side side, std::size_t levels) {
  Size total = 0;
  std::size_t n = 0;
  b.for_each_level(side, [&](Level l) {
    if (n++ < levels) total += l.size;
  });
  return total;
}

// Microprice: a mid price weighted by the size on the *opposite* side.
//   micro = (bid * ask_size + ask * bid_size) / (bid_size + ask_size)
// If buyers heavily outnumber sellers, the price is likely to tick up, so the
// microprice leans toward the ask. Returned in price units (0.0 to 1.0).
// Double is fine here: it's an output signal, never used to index the book.
template <class Book>
std::optional<double> microprice(const Book& b) {
  const Price bid = b.best_bid();
  const Price ask = b.best_ask();
  if (bid == kNoPrice || ask == kNoPrice) return std::nullopt;
  const double bs = static_cast<double>(b.size_at(Side::Buy, bid));
  const double as = static_cast<double>(b.size_at(Side::Sell, ask));
  const double micro = (to_double(bid) * as + to_double(ask) * bs) / (bs + as);
  return micro;
}

}  // namespace pm
