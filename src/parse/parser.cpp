#include "parse/parser.hpp"

#include <cstring>
#include <utility>

namespace pm {

namespace od = simdjson::ondemand;

namespace {

// Reads a JSON string value. Returns false if the value is not a string.
bool get_str(od::value& v, std::string_view& out) noexcept { return !v.get_string().get(out); }

bool get_price(od::value& v, Price& out) noexcept {
  std::string_view s;
  return get_str(v, s) && parse_price(s, out);
}

// best_bid / best_ask may be empty or null when one side of the book is empty.
bool get_optional_price(od::value& v, Price& out) noexcept {
  bool is_null = false;
  if (!v.is_null().get(is_null) && is_null) {
    out = 0;
    return true;
  }
  std::string_view s;
  if (!get_str(v, s)) return false;
  if (s.empty()) {
    out = 0;
    return true;
  }
  return parse_price(s, out);
}

bool get_size(od::value& v, Size& out) noexcept {
  std::string_view s;
  return get_str(v, s) && parse_size(s, out);
}

bool get_uint(od::value& v, std::int64_t& out) noexcept {
  std::string_view s;
  return get_str(v, s) && parse_uint(s, out);
}

bool get_side(od::value& v, Side& out) noexcept {
  std::string_view s;
  if (!get_str(v, s)) return false;
  if (s == "BUY") {
    out = Side::Buy;
  } else if (s == "SELL") {
    out = Side::Sell;
  } else {
    return false;
  }
  return true;
}

MsgType classify(std::string_view event_type) noexcept {
  if (event_type == "price_change") return MsgType::PriceChange;
  if (event_type == "book") return MsgType::Book;
  if (event_type == "last_trade_price") return MsgType::LastTrade;
  if (event_type == "tick_size_change") return MsgType::TickSizeChange;
  return MsgType::Unknown;
}

}  // namespace

ParseStatus Parser::parse(std::string_view frame, ParsedFrame& out) {
  out.clear();
  error_ = "";
  if (frame == "PONG") return ParseStatus::Pong;

  const std::size_t need = frame.size() + simdjson::SIMDJSON_PADDING;
  if (cap_ < need) {
    cap_ = need * 2;
    buf_ = std::make_unique<char[]>(cap_);
  }
  std::memcpy(buf_.get(), frame.data(), frame.size());
  std::memset(buf_.get() + frame.size(), 0, simdjson::SIMDJSON_PADDING);

  od::document doc;
  if (parser_.iterate(buf_.get(), frame.size(), cap_).get(doc)) {
    fail("not JSON");
    return ParseStatus::Error;
  }
  od::json_type type;
  if (doc.type().get(type)) {
    fail("not JSON");
    return ParseStatus::Error;
  }

  if (type == od::json_type::array) {
    od::array arr;
    if (doc.get_array().get(arr)) {
      fail("bad array");
      return ParseStatus::Error;
    }
    for (auto elem : arr) {
      od::object obj;
      if (elem.get_object().get(obj)) {
        fail("array element is not an object");
        return ParseStatus::Error;
      }
      if (!parse_message(obj, out)) return ParseStatus::Error;
    }
  } else if (type == od::json_type::object) {
    od::object obj;
    if (doc.get_object().get(obj) || !parse_message(obj, out)) {
      if (*error_ == '\0') fail("bad object");
      return ParseStatus::Error;
    }
  } else {
    fail("frame is neither object nor array");
    return ParseStatus::Error;
  }

  // On-Demand is lazy, so make sure nothing malformed trails the value.
  if (!doc.at_end()) {
    fail("trailing content after JSON value");
    return ParseStatus::Error;
  }
  return ParseStatus::Ok;
}

bool Parser::parse_message(od::object obj, ParsedFrame& out) {
  Message m;
  // Walk every field once, in whatever order the server sent them. Fields we
  // don't recognise are skipped automatically.
  for (auto field_result : obj) {
    od::field field;
    if (std::move(field_result).get(field)) return fail("bad field");
    std::string_view key;
    if (field.unescaped_key().get(key)) return fail("bad key");
    od::value v = field.value();

    bool ok = true;
    if (key == "event_type") {
      ok = get_str(v, m.event_type);
    } else if (key == "asset_id") {
      ok = get_str(v, m.asset_id);
    } else if (key == "market") {
      ok = get_str(v, m.market);
    } else if (key == "timestamp") {
      ok = get_uint(v, m.ts_ms);
    } else if (key == "hash") {
      ok = get_str(v, m.hash);
    } else if (key == "bids") {
      ok = parse_levels(v, out, m.bids);
    } else if (key == "asks") {
      ok = parse_levels(v, out, m.asks);
    } else if (key == "price_changes") {
      ok = parse_changes(v, out, m.changes);
    } else if (key == "tick_size" || key == "new_tick_size") {
      ok = get_price(v, m.tick);
    } else if (key == "old_tick_size") {
      ok = get_optional_price(v, m.old_tick);
    } else if (key == "price") {
      ok = get_price(v, m.price);
    } else if (key == "size") {
      ok = get_size(v, m.size);
    } else if (key == "side") {
      ok = get_side(v, m.side);
    } else if (key == "fee_rate_bps") {
      ok = get_uint(v, m.fee_rate_bps);
    }
    if (!ok) return error_[0] ? false : fail("bad value for a known field");
  }

  m.type = classify(m.event_type);
  switch (m.type) {
    case MsgType::Book:
      if (m.asset_id.empty()) return fail("book without asset_id");
      break;
    case MsgType::TickSizeChange:
      if (m.asset_id.empty() || m.tick == 0) return fail("tick_size_change missing fields");
      break;
    case MsgType::LastTrade:
      if (m.asset_id.empty()) return fail("last_trade_price without asset_id");
      break;
    case MsgType::PriceChange:
    case MsgType::Unknown:
      break;
  }
  out.messages.push_back(m);
  return true;
}

bool Parser::parse_levels(od::value v, ParsedFrame& out, Range& range) {
  od::array arr;
  if (v.get_array().get(arr)) return fail("bids/asks is not an array");
  range.begin = static_cast<std::uint32_t>(out.levels.size());
  for (auto elem : arr) {
    od::object obj;
    if (elem.get_object().get(obj)) return fail("level is not an object");
    Level lvl;
    bool have_price = false;
    bool have_size = false;
    for (auto field_result : obj) {
      od::field field;
      std::string_view key;
      if (std::move(field_result).get(field) || field.unescaped_key().get(key))
        return fail("bad level");
      od::value fv = field.value();
      if (key == "price") {
        if (!get_price(fv, lvl.price)) return fail("bad level price");
        have_price = true;
      } else if (key == "size") {
        if (!get_size(fv, lvl.size)) return fail("bad level size");
        have_size = true;
      }
    }
    if (!have_price || !have_size) return fail("level missing price or size");
    out.levels.push_back(lvl);
  }
  range.count = static_cast<std::uint32_t>(out.levels.size()) - range.begin;
  return true;
}

bool Parser::parse_changes(od::value v, ParsedFrame& out, Range& range) {
  od::array arr;
  if (v.get_array().get(arr)) return fail("price_changes is not an array");
  range.begin = static_cast<std::uint32_t>(out.changes.size());
  for (auto elem : arr) {
    od::object obj;
    if (elem.get_object().get(obj)) return fail("price change is not an object");
    PriceChange pc;
    bool have_price = false;
    bool have_size = false;
    bool have_side = false;
    for (auto field_result : obj) {
      od::field field;
      std::string_view key;
      if (std::move(field_result).get(field) || field.unescaped_key().get(key))
        return fail("bad change");
      od::value fv = field.value();
      bool ok = true;
      if (key == "asset_id") {
        ok = get_str(fv, pc.asset_id);
      } else if (key == "price") {
        ok = have_price = get_price(fv, pc.price);
      } else if (key == "size") {
        ok = have_size = get_size(fv, pc.size);
      } else if (key == "side") {
        ok = have_side = get_side(fv, pc.side);
      } else if (key == "hash") {
        ok = get_str(fv, pc.hash);
      } else if (key == "best_bid") {
        ok = get_optional_price(fv, pc.best_bid);
      } else if (key == "best_ask") {
        ok = get_optional_price(fv, pc.best_ask);
      }
      if (!ok) return fail("bad price change field");
    }
    if (pc.asset_id.empty() || !have_price || !have_size || !have_side) {
      return fail("price change missing asset_id/price/size/side");
    }
    out.changes.push_back(pc);
  }
  range.count = static_cast<std::uint32_t>(out.changes.size()) - range.begin;
  return true;
}

}  // namespace pm
