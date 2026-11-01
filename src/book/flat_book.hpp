#pragma once
// Order book for one asset, stored as flat arrays indexed by price.
//
// Polymarket prices are bounded: 0.0000 to 1.0000 in steps of 0.0001, which
// is 10,001 possible prices. So instead of a tree (std::map) we keep one slot
// per possible price: size_[800] is the total size resting at 0.08. Finding a
// level is one array access, with no pointer chasing and no allocation.
//
// The catch: after the best level empties, we must find the next non-empty
// slot. A plain scan could walk thousands of empty slots, so we also keep an
// occupancy bitmap (1 bit per price, 64 prices per uint64_t word) and jump
// with std::countl_zero / std::countr_zero, which compile to a single CPU
// instruction. Worst case touches 157 words, usually just one.
//
// We index by the finest possible grid (0.0001), not by the market's current
// tick. A tick_size_change then needs no re-indexing at all.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "parse/messages.hpp"

namespace pm {

inline constexpr Price kNoPrice = -1;

class FlatBook {
 public:
  static constexpr std::size_t kSlots = static_cast<std::size_t>(kPriceScale) + 1;  // 10,001
  static constexpr std::size_t kWords = (kSlots + 63) / 64;                         // 157

  FlatBook();

  void clear() noexcept;
  // Replaces the whole book. Levels may arrive in any order; size 0 is skipped.
  // Returns false if any price is out of range (those levels are skipped).
  bool apply_snapshot(std::span<const Level> bids, std::span<const Level> asks) noexcept;
  // Sets the total size at one price. Size 0 removes the level.
  bool set_level(Side side, Price price, Size size) noexcept;

  Price best_bid() const noexcept { return bids_.best; }  // kNoPrice if empty
  Price best_ask() const noexcept { return asks_.best; }
  Size size_at(Side side, Price price) const noexcept;
  std::size_t level_count(Side side) const noexcept { return of(side).count; }

  // Copies up to out.size() levels, best first. Returns how many were written.
  std::size_t top_levels(Side side, std::span<Level> out) const noexcept;

  // Calls f(Level) for every level, best first. A template so the call can be
  // inlined: no function-pointer overhead.
  template <class F>
  void for_each_level(Side side, F&& f) const {
    const SideBook& sb = of(side);
    for (Price p = sb.best; p != kNoPrice; p = next_worse(side, p)) f(Level{p, sb.size[idx(p)]});
  }

 private:
  struct SideBook {
    std::vector<Size> size;                    // kSlots entries, 0 = empty
    std::array<std::uint64_t, kWords> bits{};  // bit set <=> size != 0
    std::size_t count = 0;
    Price best = kNoPrice;
  };

  static std::size_t idx(Price p) noexcept { return static_cast<std::size_t>(p); }
  const SideBook& of(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }
  SideBook& of(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }

  // Highest occupied price <= p, or kNoPrice.
  static Price highest_at_or_below(const SideBook& sb, Price p) noexcept;
  // Lowest occupied price >= p, or kNoPrice.
  static Price lowest_at_or_above(const SideBook& sb, Price p) noexcept;
  Price next_worse(Side side, Price p) const noexcept;

  SideBook bids_;
  SideBook asks_;
};

}  // namespace pm
