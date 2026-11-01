#pragma once
// Turns book events into PMBook protocol v1 lines (see CONTRACT.md).
//
// Runs on the processing thread right after the books are updated. Lines are
// built in a reused std::string, so steady-state publishing doesn't allocate
// (except the per-asset copy of the last book line kept for new TCP clients).

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "book/book_manager.hpp"
#include "core/clock.hpp"
#include "core/json.hpp"
#include "publish/sink.hpp"

namespace pm {

class Publisher {
 public:
  explicit Publisher(Sink& sink, std::size_t depth = 10)
      : sink_(sink), levels_(depth) {}  // levels_.size() is the depth

  // Publishes everything caused by one input frame, then flushes once.
  template <class Book>
  void on_events(const std::vector<BookEvent<Book>>& events, std::int64_t recv_ns) {
    if (events.empty()) return;
    if (sink_.has_new_clients()) welcome_new_clients();
    for (const auto& e : events) {
      switch (e.kind) {
        case EventKind::BookUpdated:
          publish_book(*e.asset, e.msg->ts_ms, recv_ns);
          break;
        case EventKind::Trade:
          publish_trade(*e.asset, *e.msg, recv_ns);
          break;
        case EventKind::ResyncNeeded: {
          std::string detail = "asset " + e.asset->asset_id + ": " + e.reason;
          publish_status("resync", detail, false);
          break;
        }
      }
    }
    sink_.flush();
  }

  // state: "connected" | "reconnecting" | "resync"
  void publish_status(std::string_view state, std::string_view detail, bool flush = true) {
    line_.clear();
    line_ += R"({"v":1,"type":"status","state":")";
    line_ += state;
    line_ += R"(","detail":")";
    append_json_escaped(line_, detail);
    line_ += R"(","ts_pub_ns":)";
    append_int(line_, steady_ns());
    line_ += "}\n";
    sink_.write(line_);
    ++lines_;
    if (flush) sink_.flush();
  }

  std::uint64_t seq(std::string_view asset_id) const {
    for (const auto& [ptr, st] : per_asset_) {
      if (st.asset_id == asset_id) return st.seq;
    }
    return 0;
  }
  std::uint64_t lines_published() const noexcept { return lines_; }

 private:
  struct AssetPub {
    std::string asset_id;
    std::uint64_t seq = 0;
    std::string last_book;  // replayed to TCP clients that connect later
  };

  template <class Book>
  AssetPub& state_for(const AssetBook<Book>& a) {
    auto [it, inserted] = per_asset_.try_emplace(&a);
    if (inserted) it->second.asset_id = a.asset_id;
    return it->second;
  }

  template <class Book>
  void append_levels(const Book& book, Side side) {
    const std::size_t n = book.top_levels(side, levels_);
    line_ += '[';
    char buf[24];
    for (std::size_t i = 0; i < n; ++i) {
      if (i) line_ += ',';
      line_ += '[';
      line_.append(buf, format_fixed(levels_[i].price, kPriceDecimals, buf));
      line_ += ',';
      line_.append(buf, format_fixed(levels_[i].size, kSizeDecimals, buf));
      line_ += ']';
    }
    line_ += ']';
  }

  template <class Book>
  void publish_book(const AssetBook<Book>& a, std::int64_t ts_exchange_ms, std::int64_t recv_ns) {
    AssetPub& st = state_for(a);
    char buf[24];
    line_.clear();
    line_ += R"({"v":1,"type":"book","asset_id":")";
    line_ += a.asset_id;
    line_ += R"(","seq":)";
    append_int(line_, static_cast<std::int64_t>(++st.seq));
    line_ += R"(,"ts_exchange_ms":)";
    append_int(line_, ts_exchange_ms);
    line_ += R"(,"ts_recv_ns":)";
    append_int(line_, recv_ns);
    line_ += R"(,"ts_pub_ns":)";
    const std::size_t pub_pos = line_.size();
    line_ += "0000000000000000000";  // placeholder, overwritten just before writing
    line_ += R"(,"tick":)";
    line_.append(buf, format_fixed(a.tick, kPriceDecimals, buf));
    line_ += R"(,"bids":)";
    append_levels(*a.book, Side::Buy);
    line_ += R"(,"asks":)";
    append_levels(*a.book, Side::Sell);
    line_ += "}\n";
    // Take the publish timestamp as late as possible: after formatting.
    stamp(pub_pos, steady_ns());
    sink_.write(line_);
    st.last_book.assign(line_);
    ++lines_;
  }

  template <class Book>
  void publish_trade(const AssetBook<Book>& a, const Message& m, std::int64_t recv_ns) {
    AssetPub& st = state_for(a);
    char buf[24];
    line_.clear();
    line_ += R"({"v":1,"type":"trade","asset_id":")";
    line_ += a.asset_id;
    line_ += R"(","seq":)";
    append_int(line_, static_cast<std::int64_t>(++st.seq));
    line_ += R"(,"ts_exchange_ms":)";
    append_int(line_, m.ts_ms);
    line_ += R"(,"ts_recv_ns":)";
    append_int(line_, recv_ns);
    line_ += R"(,"price":)";
    line_.append(buf, format_fixed(m.price, kPriceDecimals, buf));
    line_ += R"(,"size":)";
    line_.append(buf, format_fixed(m.size, kSizeDecimals, buf));
    line_ += m.side == Side::Buy ? R"(,"side":"BUY"})" : R"(,"side":"SELL"})";
    line_ += '\n';
    sink_.write(line_);
    ++lines_;
  }

  // Replaces the 19-character placeholder with the real timestamp digits.
  // (We can't zero-pad in place: JSON numbers may not have leading zeros.)
  void stamp(std::size_t pos, std::int64_t ns) {
    char buf[24];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, ns);
    const std::size_t len = static_cast<std::size_t>(end - buf);
    line_.replace(pos, 19, buf, len);
  }

  void welcome_new_clients() {
    std::vector<std::string_view> lines;
    for (const auto& [ptr, st] : per_asset_) {
      if (!st.last_book.empty()) lines.push_back(st.last_book);
    }
    sink_.adopt_new_clients(lines);
  }

  Sink& sink_;
  std::vector<Level> levels_;
  std::string line_;
  std::unordered_map<const void*, AssetPub> per_asset_;  // keyed by AssetBook address (stable)
  std::uint64_t lines_ = 0;
};

}  // namespace pm
