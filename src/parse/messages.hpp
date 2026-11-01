#pragma once
// Compact C++ versions of Polymarket market-channel messages.
//
// One WebSocket frame can hold several messages (the first book snapshots
// arrive as a JSON array), so the parser fills a ParsedFrame. Variable-length
// parts (book levels, price changes) live in shared vectors inside the frame,
// and each Message points at its slice with a Range. The vectors are cleared
// but not freed between frames, so once warmed up, parsing allocates nothing.
//
// LIFETIME RULE: every std::string_view in here points into the Parser's
// internal buffers. They are valid only until the next Parser::parse() call.
// Copy anything you need to keep.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "parse/fixed_point.hpp"

namespace pm {

enum class Side : std::uint8_t { Buy, Sell };

enum class MsgType : std::uint8_t {
  Book,            // full snapshot of one asset's book
  PriceChange,     // one or more level updates
  TickSizeChange,  // minimum price step changed
  LastTrade,       // a trade happened
  Unknown,         // event_type we don't handle (kept so callers can count it)
};

struct Level {
  Price price = 0;
  Size size = 0;  // 0 in a delta means "remove this level"
};

struct PriceChange {
  std::string_view asset_id;
  std::string_view hash;
  Price price = 0;
  Size size = 0;
  Side side = Side::Buy;  // BUY updates the bid side, SELL the ask side
  Price best_bid = 0;     // exchange's view after the change; 0 = none / absent
  Price best_ask = 0;
};

// A [begin, begin + count) slice of one of ParsedFrame's vectors.
struct Range {
  std::uint32_t begin = 0;
  std::uint32_t count = 0;
};

struct Message {
  MsgType type = MsgType::Unknown;
  std::string_view event_type;
  std::string_view market;    // condition ID (0x...)
  std::string_view asset_id;  // Book, TickSizeChange, LastTrade
  std::int64_t ts_ms = 0;     // exchange timestamp, ms since epoch (0 if absent)

  // Book
  std::string_view hash;
  Range bids;
  Range asks;

  // Book (`tick_size`, 0 if absent) and TickSizeChange (`new_tick_size`)
  Price tick = 0;
  Price old_tick = 0;  // TickSizeChange

  // PriceChange
  Range changes;

  // LastTrade
  Price price = 0;
  Size size = 0;
  Side side = Side::Buy;
  std::int64_t fee_rate_bps = 0;
};

struct ParsedFrame {
  std::vector<Message> messages;
  std::vector<Level> levels;
  std::vector<PriceChange> changes;

  void clear() noexcept {
    messages.clear();
    levels.clear();
    changes.clear();
  }

  // std::span is a non-owning "pointer + length" view over contiguous memory,
  // like string_view but for any element type.
  std::span<const Level> bids(const Message& m) const noexcept { return slice(levels, m.bids); }
  std::span<const Level> asks(const Message& m) const noexcept { return slice(levels, m.asks); }
  std::span<const PriceChange> price_changes(const Message& m) const noexcept {
    return slice(changes, m.changes);
  }

 private:
  template <class T>
  static std::span<const T> slice(const std::vector<T>& v, Range r) noexcept {
    return std::span<const T>(v).subspan(r.begin, r.count);
  }
};

}  // namespace pm
