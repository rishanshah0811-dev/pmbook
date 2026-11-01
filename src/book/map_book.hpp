#pragma once
// The "obvious" order book: one balanced binary tree (std::map) per side.
// Simple and clearly correct, so we use it as the reference that FlatBook
// is checked against, and as the naive baseline in benchmarks.
//
// Why it's slower: every map node is a separate heap allocation, so walking
// or searching the tree jumps between scattered memory addresses (cache
// misses), and inserting a new level calls malloc.

#include <cstddef>
#include <functional>
#include <map>
#include <span>

#include "book/flat_book.hpp"  // kNoPrice
#include "parse/messages.hpp"

namespace pm {

class MapBook {
 public:
  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  bool apply_snapshot(std::span<const Level> bids, std::span<const Level> asks) {
    clear();
    bool ok = true;
    for (const Level& l : bids) ok &= set_level(Side::Buy, l.price, l.size);
    for (const Level& l : asks) ok &= set_level(Side::Sell, l.price, l.size);
    return ok;
  }

  bool set_level(Side side, Price price, Size size) {
    if (price < 0 || price > kPriceScale || size < 0) return false;
    if (side == Side::Buy) {
      update(bids_, price, size);
    } else {
      update(asks_, price, size);
    }
    return true;
  }

  Price best_bid() const noexcept { return bids_.empty() ? kNoPrice : bids_.begin()->first; }
  Price best_ask() const noexcept { return asks_.empty() ? kNoPrice : asks_.begin()->first; }

  Size size_at(Side side, Price price) const {
    if (side == Side::Buy) return lookup(bids_, price);
    return lookup(asks_, price);
  }

  std::size_t level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }

  std::size_t top_levels(Side side, std::span<Level> out) const {
    return side == Side::Buy ? copy_top(bids_, out) : copy_top(asks_, out);
  }

  template <class F>
  void for_each_level(Side side, F&& f) const {
    if (side == Side::Buy) {
      for (const auto& [p, s] : bids_) f(Level{p, s});
    } else {
      for (const auto& [p, s] : asks_) f(Level{p, s});
    }
  }

 private:
  template <class M>
  static void update(M& m, Price price, Size size) {
    if (size == 0) {
      m.erase(price);
    } else {
      m[price] = size;
    }
  }
  // Stops after out.size() levels, so the baseline is not unfairly slow.
  template <class M>
  static std::size_t copy_top(const M& m, std::span<Level> out) {
    std::size_t n = 0;
    for (auto it = m.begin(); it != m.end() && n < out.size(); ++it)
      out[n++] = Level{it->first, it->second};
    return n;
  }
  template <class M>
  static Size lookup(const M& m, Price price) {
    auto it = m.find(price);
    return it == m.end() ? 0 : it->second;
  }

  std::map<Price, Size, std::greater<>> bids_;  // highest price first
  std::map<Price, Size> asks_;                  // lowest price first
};

}  // namespace pm
