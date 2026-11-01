#pragma once
// Keeps one order book per asset and applies parsed messages to them.
//
// It also checks that our copy stays correct. Polymarket's market channel has
// no sequence numbers, so we can't spot a missed message directly. Instead:
//   * every price_change carries the exchange's best_bid/best_ask *after* the
//     change; if ours differ, our book is wrong;
//   * a crossed book (bid >= ask) means our book is wrong;
//   * a delta for an asset we have no snapshot for can't be applied.
// Any of these marks the asset NeedsResync and emits a ResyncNeeded event.
// The feed layer reacts (resubscribe -> fresh `book` snapshot), and the next
// snapshot makes the asset Live again.
//
// Templated on the book type so the naive std::map version can reuse it.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "book/book_math.hpp"
#include "book/flat_book.hpp"
#include "parse/messages.hpp"

namespace pm {

enum class AssetState : std::uint8_t {
  Empty,        // no snapshot yet
  Live,         // book is believed correct
  NeedsResync,  // inconsistency seen; ignore deltas until the next snapshot
};

template <class Book>
struct AssetBook {
  std::string asset_id;
  // FlatBook is ~160 KB, so keep it on the heap; moving AssetBook stays cheap.
  std::unique_ptr<Book> book = std::make_unique<Book>();
  AssetState state = AssetState::Empty;
  Price tick = 0;               // current tick size (0 if unknown)
  std::int64_t last_ts_ms = 0;  // exchange timestamp of last applied message
  std::uint64_t updates = 0;    // messages that changed this book
  std::uint64_t resyncs = 0;    // times we flagged NeedsResync
};

enum class EventKind : std::uint8_t {
  BookUpdated,   // this asset's book changed (snapshot or delta)
  Trade,         // last_trade_price for this asset (msg points to it)
  ResyncNeeded,  // asset went NeedsResync (reason says why)
};

template <class Book>
struct BookEvent {
  EventKind kind;
  AssetBook<Book>* asset;
  const Message* msg;  // the message that caused it (valid until next parse)
  const char* reason;  // static string, only for ResyncNeeded
};

struct ManagerStats {
  std::uint64_t snapshots = 0;
  std::uint64_t deltas = 0;
  std::uint64_t trades = 0;
  std::uint64_t ignored_deltas = 0;  // asset Empty/NeedsResync/unknown
  std::uint64_t bbo_mismatches = 0;
  std::uint64_t crossed = 0;
  std::uint64_t unknown_messages = 0;
};

template <class Book>
class BookManager {
 public:
  using Asset = AssetBook<Book>;
  using Event = BookEvent<Book>;

  // Applies every message in `frame` in order. Appends events to `out`.
  void apply(const ParsedFrame& frame, std::vector<Event>& out) {
    for (const Message& m : frame.messages) {
      switch (m.type) {
        case MsgType::Book:
          on_book(frame, m, out);
          break;
        case MsgType::PriceChange:
          on_price_change(frame, m, out);
          break;
        case MsgType::TickSizeChange:
          get_or_create(m.asset_id).tick = m.tick;
          break;
        case MsgType::LastTrade:
          ++stats_.trades;
          out.push_back(Event{EventKind::Trade, &get_or_create(m.asset_id), &m, ""});
          break;
        case MsgType::Unknown:
          ++stats_.unknown_messages;
          break;
      }
    }
  }

  // Marks every asset as needing a fresh snapshot (e.g. after a reconnect,
  // when we may have missed messages while disconnected).
  void invalidate_all() {
    for (auto& [id, a] : assets_) {
      if (a.state == AssetState::Live) a.state = AssetState::NeedsResync;
    }
  }

  const Asset* find(std::string_view asset_id) const {
    auto it = assets_.find(asset_id);
    return it == assets_.end() ? nullptr : &it->second;
  }
  const auto& assets() const noexcept { return assets_; }
  const ManagerStats& stats() const noexcept { return stats_; }

 private:
  void on_book(const ParsedFrame& frame, const Message& m, std::vector<Event>& out) {
    ++stats_.snapshots;
    Asset& a = get_or_create(m.asset_id);
    a.book->apply_snapshot(frame.bids(m), frame.asks(m));
    if (m.tick != 0) a.tick = m.tick;
    a.last_ts_ms = m.ts_ms;
    ++a.updates;
    a.state = AssetState::Live;
    out.push_back(Event{EventKind::BookUpdated, &a, &m, ""});
    if (is_crossed(*a.book)) {
      ++stats_.crossed;
      flag(a, m, "crossed book after snapshot", out);
    }
  }

  void on_price_change(const ParsedFrame& frame, const Message& m, std::vector<Event>& out) {
    // One message can touch several assets (YES and NO of one market). Emit at
    // most one BookUpdated per asset per message, after all its changes.
    touched_.clear();
    for (const PriceChange& pc : frame.price_changes(m)) {
      ++stats_.deltas;
      auto it = assets_.find(pc.asset_id);
      if (it == assets_.end() || it->second.state != AssetState::Live) {
        ++stats_.ignored_deltas;
        continue;
      }
      Asset& a = it->second;
      a.book->set_level(pc.side, pc.price, pc.size);
      a.last_ts_ms = m.ts_ms;

      if (pc.has_bbo && !bbo_matches(*a.book, pc)) {
        ++stats_.bbo_mismatches;
        flag(a, m, "best bid/ask differs from exchange", out);
        continue;
      }
      if (is_crossed(*a.book)) {
        ++stats_.crossed;
        flag(a, m, "crossed book after delta", out);
        continue;
      }
      bool seen = false;
      for (Asset* t : touched_) seen |= (t == &a);
      if (!seen) touched_.push_back(&a);
    }
    for (Asset* a : touched_) {
      if (a->state != AssetState::Live) continue;  // flagged by a later change
      ++a->updates;
      out.push_back(Event{EventKind::BookUpdated, a, &m, ""});
    }
  }

  static bool bbo_matches(const Book& b, const PriceChange& pc) noexcept {
    // The exchange sends 0/"" for an empty side; we use kNoPrice.
    const Price bid = b.best_bid() == kNoPrice ? 0 : b.best_bid();
    const Price ask = b.best_ask() == kNoPrice ? 0 : b.best_ask();
    return bid == pc.best_bid && ask == pc.best_ask;
  }

  void flag(Asset& a, const Message& m, const char* reason, std::vector<Event>& out) {
    a.state = AssetState::NeedsResync;
    ++a.resyncs;
    out.push_back(Event{EventKind::ResyncNeeded, &a, &m, reason});
  }

  Asset& get_or_create(std::string_view id) {
    auto it = assets_.find(id);
    if (it != assets_.end()) return it->second;
    Asset a;
    a.asset_id = std::string(id);
    return assets_.emplace(a.asset_id, std::move(a)).first->second;
  }

  // Transparent hashing lets us look up with a string_view key without first
  // building a std::string (which would allocate on every message).
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::unordered_map<std::string, Asset, StringHash, std::equal_to<>> assets_;
  std::vector<Asset*> touched_;
  ManagerStats stats_;
};

}  // namespace pm
