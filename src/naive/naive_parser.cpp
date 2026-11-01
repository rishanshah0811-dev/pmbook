#include "naive/naive_parser.hpp"

namespace pm {

namespace {

using json = nlohmann::json;

bool str(const json& o, const char* key, std::string_view& out) {
  auto it = o.find(key);
  if (it == o.end()) return true;  // absent is fine; required fields are checked later
  if (!it->is_string()) return false;
  out = it->get_ref<const std::string&>();
  return true;
}

template <class F>
bool decimal(const json& o, const char* key, F&& convert) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  if (!it->is_string()) return false;
  return convert(std::string_view(it->get_ref<const std::string&>()));
}

bool side_of(std::string_view s, Side& out) {
  if (s == "BUY") {
    out = Side::Buy;
  } else if (s == "SELL") {
    out = Side::Sell;
  } else {
    return false;
  }
  return true;
}

bool optional_price(const json& o, const char* key, Price& out, bool& present) {
  auto it = o.find(key);
  if (it == o.end()) return true;
  present = true;
  if (it->is_null()) {
    out = 0;
    return true;
  }
  if (!it->is_string()) return false;
  const auto& s = it->get_ref<const std::string&>();
  if (s.empty()) {
    out = 0;
    return true;
  }
  return parse_price(s, out);
}

MsgType classify(std::string_view t) {
  if (t == "price_change") return MsgType::PriceChange;
  if (t == "book") return MsgType::Book;
  if (t == "last_trade_price") return MsgType::LastTrade;
  if (t == "tick_size_change") return MsgType::TickSizeChange;
  return MsgType::Unknown;
}

}  // namespace

ParseStatus NaiveParser::parse(std::string_view frame, ParsedFrame& out) {
  out.clear();
  error_ = "";
  if (frame == "PONG") return ParseStatus::Pong;
  doc_ = json::parse(frame.begin(), frame.end(), nullptr, /*allow_exceptions=*/false);
  if (doc_.is_discarded()) {
    fail("not JSON");
    return ParseStatus::Error;
  }
  if (doc_.is_array()) {
    for (const auto& o : doc_) {
      if (!o.is_object()) {
        fail("array element is not an object");
        return ParseStatus::Error;
      }
      if (!parse_message(o, out)) return ParseStatus::Error;
    }
    return ParseStatus::Ok;
  }
  if (doc_.is_object()) return parse_message(doc_, out) ? ParseStatus::Ok : ParseStatus::Error;
  fail("frame is neither object nor array");
  return ParseStatus::Error;
}

bool NaiveParser::parse_message(const json& o, ParsedFrame& out) {
  Message m;
  std::string_view side;
  bool ok = str(o, "event_type", m.event_type) && str(o, "asset_id", m.asset_id) &&
            str(o, "market", m.market) && str(o, "hash", m.hash) && str(o, "side", side) &&
            decimal(o, "timestamp", [&](std::string_view s) { return parse_uint(s, m.ts_ms); }) &&
            decimal(o, "price", [&](std::string_view s) { return parse_price(s, m.price); }) &&
            decimal(o, "size", [&](std::string_view s) { return parse_size(s, m.size); }) &&
            decimal(o, "fee_rate_bps",
                    [&](std::string_view s) { return parse_uint(s, m.fee_rate_bps); }) &&
            decimal(o, "tick_size", [&](std::string_view s) { return parse_price(s, m.tick); }) &&
            decimal(o, "new_tick_size", [&](std::string_view s) { return parse_price(s, m.tick); });
  bool unused = false;
  ok = ok && optional_price(o, "old_tick_size", m.old_tick, unused);
  if (!side.empty()) ok = ok && side_of(side, m.side);
  if (!ok) return fail("bad value for a known field");

  for (const char* key : {"bids", "asks"}) {
    auto it = o.find(key);
    if (it == o.end()) continue;
    if (!it->is_array()) return fail("bids/asks is not an array");
    Range& r = key[0] == 'b' ? m.bids : m.asks;
    r.begin = static_cast<std::uint32_t>(out.levels.size());
    for (const auto& lv : *it) {
      Level l;
      if (!lv.is_object() || !lv.contains("price") || !lv.contains("size"))
        return fail("bad level");
      if (!decimal(lv, "price", [&](std::string_view s) { return parse_price(s, l.price); }) ||
          !decimal(lv, "size", [&](std::string_view s) { return parse_size(s, l.size); })) {
        return fail("bad level value");
      }
      out.levels.push_back(l);
    }
    r.count = static_cast<std::uint32_t>(out.levels.size()) - r.begin;
  }

  if (auto it = o.find("price_changes"); it != o.end()) {
    if (!it->is_array()) return fail("price_changes is not an array");
    m.changes.begin = static_cast<std::uint32_t>(out.changes.size());
    for (const auto& c : *it) {
      if (!c.is_object()) return fail("price change is not an object");
      PriceChange pc;
      std::string_view pside;
      bool have_bid = false, have_ask = false;
      if (!str(c, "asset_id", pc.asset_id) || !str(c, "hash", pc.hash) || !str(c, "side", pside) ||
          !decimal(c, "price", [&](std::string_view s) { return parse_price(s, pc.price); }) ||
          !decimal(c, "size", [&](std::string_view s) { return parse_size(s, pc.size); }) ||
          !optional_price(c, "best_bid", pc.best_bid, have_bid) ||
          !optional_price(c, "best_ask", pc.best_ask, have_ask)) {
        return fail("bad price change field");
      }
      if (pc.asset_id.empty() || !c.contains("price") || !c.contains("size") ||
          !side_of(pside, pc.side)) {
        return fail("price change missing asset_id/price/size/side");
      }
      pc.has_bbo = have_bid && have_ask;
      out.changes.push_back(pc);
    }
    m.changes.count = static_cast<std::uint32_t>(out.changes.size()) - m.changes.begin;
  }

  m.type = classify(m.event_type);
  if ((m.type == MsgType::Book || m.type == MsgType::LastTrade) && m.asset_id.empty()) {
    return fail("missing asset_id");
  }
  if (m.type == MsgType::TickSizeChange && (m.asset_id.empty() || m.tick == 0)) {
    return fail("tick_size_change missing fields");
  }
  out.messages.push_back(m);
  return true;
}

}  // namespace pm
