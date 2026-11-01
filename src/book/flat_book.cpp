#include "book/flat_book.hpp"

#include <bit>

namespace pm {

FlatBook::FlatBook() {
  bids_.size.assign(kSlots, 0);
  asks_.size.assign(kSlots, 0);
}

void FlatBook::clear() noexcept {
  // Only zero the slots that are actually occupied (found via the bitmap),
  // instead of rewriting all 10,001 entries.
  for (SideBook* sb : {&bids_, &asks_}) {
    for (std::size_t w = 0; w < kWords; ++w) {
      std::uint64_t word = sb->bits[w];
      while (word != 0) {
        const auto bit = static_cast<std::size_t>(std::countr_zero(word));
        sb->size[w * 64 + bit] = 0;
        word &= word - 1;  // clear lowest set bit
      }
      sb->bits[w] = 0;
    }
    sb->count = 0;
    sb->best = kNoPrice;
  }
}

bool FlatBook::apply_snapshot(std::span<const Level> bids, std::span<const Level> asks) noexcept {
  clear();
  bool ok = true;
  for (const Level& l : bids) ok &= set_level(Side::Buy, l.price, l.size);
  for (const Level& l : asks) ok &= set_level(Side::Sell, l.price, l.size);
  return ok;
}

bool FlatBook::set_level(Side side, Price price, Size size) noexcept {
  if (price < 0 || price > kPriceScale || size < 0) return false;
  SideBook& sb = of(side);
  const std::size_t i = idx(price);
  const Size old = sb.size[i];
  sb.size[i] = size;

  const std::uint64_t mask = std::uint64_t{1} << (i & 63);
  if (old == 0 && size != 0) {  // new level
    sb.bits[i >> 6] |= mask;
    ++sb.count;
    if (side == Side::Buy) {
      if (sb.best == kNoPrice || price > sb.best) sb.best = price;
    } else {
      if (sb.best == kNoPrice || price < sb.best) sb.best = price;
    }
  } else if (old != 0 && size == 0) {  // level removed
    sb.bits[i >> 6] &= ~mask;
    --sb.count;
    if (price == sb.best) {
      sb.best = side == Side::Buy ? highest_at_or_below(sb, price) : lowest_at_or_above(sb, price);
    }
  }
  return true;
}

Size FlatBook::size_at(Side side, Price price) const noexcept {
  if (price < 0 || price > kPriceScale) return 0;
  return of(side).size[idx(price)];
}

std::size_t FlatBook::top_levels(Side side, std::span<Level> out) const noexcept {
  const SideBook& sb = of(side);
  std::size_t n = 0;
  for (Price p = sb.best; p != kNoPrice && n < out.size(); p = next_worse(side, p)) {
    out[n++] = Level{p, sb.size[idx(p)]};
  }
  return n;
}

Price FlatBook::next_worse(Side side, Price p) const noexcept {
  if (side == Side::Buy) return p == 0 ? kNoPrice : highest_at_or_below(bids_, p - 1);
  return p == kPriceScale ? kNoPrice : lowest_at_or_above(asks_, p + 1);
}

Price FlatBook::highest_at_or_below(const SideBook& sb, Price p) noexcept {
  if (p < 0) return kNoPrice;
  std::size_t w = idx(p) >> 6;
  const unsigned bit = static_cast<unsigned>(p & 63);
  // Keep only bits 0..bit of the first word.
  std::uint64_t word =
      sb.bits[w] & (bit == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bit + 1)) - 1));
  while (true) {
    if (word != 0)
      return static_cast<Price>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(word)));
    if (w == 0) return kNoPrice;
    word = sb.bits[--w];
  }
}

Price FlatBook::lowest_at_or_above(const SideBook& sb, Price p) noexcept {
  if (p > kPriceScale) return kNoPrice;
  std::size_t w = idx(p) >> 6;
  std::uint64_t word = sb.bits[w] & (~std::uint64_t{0} << (p & 63));
  while (true) {
    if (word != 0)
      return static_cast<Price>(w * 64 + static_cast<std::size_t>(std::countr_zero(word)));
    if (++w == kWords) return kNoPrice;
    word = sb.bits[w];
  }
}

}  // namespace pm
