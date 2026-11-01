#pragma once
// End-of-run summary: per-asset final book state plus counters, written as
// JSON. A live run and a replay of its recording should produce identical
// per-asset state hashes (milestone 5 checks exactly that).

#include <cstdio>
#include <string>

#include "book/book_manager.hpp"
#include "record/recorder.hpp"

namespace pm {

template <class Book>
std::string summary_json(const BookManager<Book>& mgr, std::uint64_t frames) {
  const ManagerStats& s = mgr.stats();
  std::string out =
      "{\"frames\":" + std::to_string(frames) + ",\"snapshots\":" + std::to_string(s.snapshots) +
      ",\"deltas\":" + std::to_string(s.deltas) + ",\"trades\":" + std::to_string(s.trades) +
      ",\"ignored_deltas\":" + std::to_string(s.ignored_deltas) +
      ",\"bbo_mismatches\":" + std::to_string(s.bbo_mismatches) +
      ",\"crossed\":" + std::to_string(s.crossed) + ",\"assets\":{";
  bool first = true;
  for (const auto& [id, a] : mgr.assets()) {
    if (!first) out += ',';
    first = false;
    char hash[19];
    std::snprintf(hash, sizeof hash, "%016llx",
                  static_cast<unsigned long long>(state_hash(*a.book)));
    out += "\"" + id + "\":{\"state\":\"" +
           (a.state == AssetState::Live    ? "live"
            : a.state == AssetState::Empty ? "empty"
                                           : "resync") +
           "\",\"best_bid\":" + std::to_string(a.book->best_bid()) +
           ",\"best_ask\":" + std::to_string(a.book->best_ask()) +
           ",\"bid_levels\":" + std::to_string(a.book->level_count(Side::Buy)) +
           ",\"ask_levels\":" + std::to_string(a.book->level_count(Side::Sell)) +
           ",\"updates\":" + std::to_string(a.updates) +
           ",\"resyncs\":" + std::to_string(a.resyncs) + ",\"state_hash\":\"" + hash + "\"}";
  }
  out += "}}";
  return out;
}

}  // namespace pm
