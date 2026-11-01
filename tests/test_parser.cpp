// Parser tests. Example frames are copied from the official docs
// (docs.polymarket.com, market channel, "API" tab) and from a real live
// capture (tests/data/live_frames.jsonl, recorded 2026-11-01).
#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "parse/parser.hpp"

using namespace pm;

namespace {

constexpr std::string_view kAsset =
    "107505882767731489358349912513945399560393482969656700824895970500493757150417";

// --- Frames copied verbatim from the docs ---------------------------------

constexpr std::string_view kDocsBook = R"({
  "event_type": "book",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "timestamp": "1782753357257",
  "hash": "0xabc123…",
  "bids": [
    { "price": "0.08", "size": "33343.4" },
    { "price": "0.09", "size": "163939.58" }
  ],
  "asks": [
    { "price": "0.99", "size": "218442.27" },
    { "price": "0.98", "size": "13229.55" }
  ]
})";

constexpr std::string_view kDocsPriceChange = R"({
  "event_type": "price_change",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "price_changes": [
    {
      "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
      "price": "0.08",
      "size": "33343.4",
      "side": "BUY",
      "hash": "56621a121a47ed9333273e21c83b660cff37ae50",
      "best_bid": "0.08",
      "best_ask": "0.09"
    }
  ],
  "timestamp": "1782753357257"
})";

constexpr std::string_view kDocsLastTrade = R"({
  "event_type": "last_trade_price",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "price": "0.08",
  "size": "219.217767",
  "fee_rate_bps": "0",
  "side": "SELL",
  "timestamp": "1782753357257",
  "transaction_hash": "0xeeefff…"
})";

constexpr std::string_view kDocsTickSize = R"({
  "event_type": "tick_size_change",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "old_tick_size": "0.01",
  "new_tick_size": "0.001",
  "timestamp": "1782753357257"
})";

// Only sent with custom_feature_enabled; we should skip it, not fail.
constexpr std::string_view kDocsBestBidAsk = R"({
  "event_type": "best_bid_ask",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "best_bid": "0.08", "best_ask": "0.09", "spread": "0.01",
  "timestamp": "1782753357257"
})";

struct Fixture : ::testing::Test {
  Parser parser;
  ParsedFrame frame;

  const Message& parse_one(std::string_view json) {
    EXPECT_EQ(parser.parse(json, frame), ParseStatus::Ok) << parser.last_error();
    EXPECT_EQ(frame.messages.size(), 1u);
    return frame.messages.at(0);
  }
  ParseStatus status(std::string_view json) { return parser.parse(json, frame); }
};

}  // namespace

TEST_F(Fixture, DocsBook) {
  const Message& m = parse_one(kDocsBook);
  EXPECT_EQ(m.type, MsgType::Book);
  EXPECT_EQ(m.asset_id, kAsset);
  EXPECT_EQ(m.hash, "0xabc123…");  // UTF-8 survives untouched
  EXPECT_EQ(m.ts_ms, 1'782'753'357'257);
  EXPECT_EQ(m.tick, 0);  // the docs example has no tick_size field

  auto bids = frame.bids(m);
  ASSERT_EQ(bids.size(), 2u);
  // Parser keeps the server's order; sorting is the book's job.
  EXPECT_EQ(bids[0].price, 800);
  EXPECT_EQ(bids[0].size, 33'343'400'000);
  EXPECT_EQ(bids[1].price, 900);
  EXPECT_EQ(bids[1].size, 163'939'580'000);

  auto asks = frame.asks(m);
  ASSERT_EQ(asks.size(), 2u);
  EXPECT_EQ(asks[0].price, 9900);
  EXPECT_EQ(asks[1].price, 9800);
  EXPECT_EQ(asks[1].size, 13'229'550'000);
}

TEST_F(Fixture, DocsPriceChange) {
  const Message& m = parse_one(kDocsPriceChange);
  EXPECT_EQ(m.type, MsgType::PriceChange);
  EXPECT_EQ(m.ts_ms, 1'782'753'357'257);
  auto changes = frame.price_changes(m);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].asset_id, kAsset);
  EXPECT_EQ(changes[0].price, 800);
  EXPECT_EQ(changes[0].size, 33'343'400'000);
  EXPECT_EQ(changes[0].side, Side::Buy);
  EXPECT_EQ(changes[0].hash, "56621a121a47ed9333273e21c83b660cff37ae50");
  EXPECT_EQ(changes[0].best_bid, 800);
  EXPECT_EQ(changes[0].best_ask, 900);
}

TEST_F(Fixture, DocsLastTrade) {
  const Message& m = parse_one(kDocsLastTrade);
  EXPECT_EQ(m.type, MsgType::LastTrade);
  EXPECT_EQ(m.asset_id, kAsset);
  EXPECT_EQ(m.price, 800);
  EXPECT_EQ(m.size, 219'217'767);
  EXPECT_EQ(m.side, Side::Sell);
  EXPECT_EQ(m.fee_rate_bps, 0);
}

TEST_F(Fixture, DocsTickSizeChange) {
  const Message& m = parse_one(kDocsTickSize);
  EXPECT_EQ(m.type, MsgType::TickSizeChange);
  EXPECT_EQ(m.old_tick, 100);
  EXPECT_EQ(m.tick, 10);
}

TEST_F(Fixture, UnknownEventTypeIsKeptNotAnError) {
  const Message& m = parse_one(kDocsBestBidAsk);
  EXPECT_EQ(m.type, MsgType::Unknown);
  EXPECT_EQ(m.event_type, "best_bid_ask");
}

TEST_F(Fixture, Pong) {
  EXPECT_EQ(status("PONG"), ParseStatus::Pong);
  EXPECT_TRUE(frame.messages.empty());
}

TEST_F(Fixture, EmptyBook) {
  const Message& m = parse_one(R"({"event_type":"book","asset_id":"1","bids":[],"asks":[]})");
  EXPECT_EQ(m.type, MsgType::Book);
  EXPECT_TRUE(frame.bids(m).empty());
  EXPECT_TRUE(frame.asks(m).empty());
}

TEST_F(Fixture, SizeZeroMeansRemoveAndIsPreserved) {
  const Message& m = parse_one(
      R"({"event_type":"price_change","price_changes":[{"asset_id":"1","price":"0.5","size":"0","side":"SELL","best_bid":"0.4","best_ask":"0.6"}]})");
  auto c = frame.price_changes(m);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0].size, 0);
  EXPECT_EQ(c[0].side, Side::Sell);
}

TEST_F(Fixture, EmptyOrNullBestBidMeansNone) {
  const Message& m = parse_one(
      R"({"event_type":"price_change","price_changes":[{"asset_id":"1","price":"0.5","size":"10","side":"BUY","best_bid":"","best_ask":null}]})");
  auto c = frame.price_changes(m);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0].best_bid, 0);
  EXPECT_EQ(c[0].best_ask, 0);
}

TEST_F(Fixture, ArrayFrameOfSeveralBooks) {
  ASSERT_EQ(
      status(R"([{"event_type":"book","asset_id":"A","bids":[{"price":"0.1","size":"1"}],"asks":[]},
                      {"event_type":"book","asset_id":"B","bids":[],"asks":[{"price":"0.9","size":"2"},{"price":"0.8","size":"3"}]}])"),
      ParseStatus::Ok);
  ASSERT_EQ(frame.messages.size(), 2u);
  EXPECT_EQ(frame.messages[0].asset_id, "A");
  EXPECT_EQ(frame.bids(frame.messages[0]).size(), 1u);
  EXPECT_EQ(frame.messages[1].asset_id, "B");
  EXPECT_EQ(frame.asks(frame.messages[1]).size(), 2u);
  EXPECT_EQ(frame.asks(frame.messages[1])[1].size, 3'000'000);
}

TEST_F(Fixture, ReusingTheFrameClearsOldMessages) {
  parse_one(kDocsBook);
  parse_one(kDocsTickSize);
  EXPECT_TRUE(frame.levels.empty());
}

TEST_F(Fixture, RejectsMalformedFrames) {
  for (
      std::string_view bad : {
          std::string_view{"not json"},
          std::string_view{"{"},
          std::string_view{"42"},
          std::string_view{R"({"event_type":"book"})"},  // no asset_id
          std::string_view{
              R"({"event_type":"book","asset_id":"1","bids":[{"price":"1.5","size":"1"}]})"},
          std::string_view{R"({"event_type":"book","asset_id":"1","bids":[{"price":"0.5"}]})"},
          std::string_view{R"({"event_type":"book","asset_id":"1","bids":{}})"},
          std::string_view{
              R"({"event_type":"book","asset_id":"1","bids":[{"price":0.5,"size":"1"}]})"},
          std::string_view{
              R"({"event_type":"price_change","price_changes":[{"asset_id":"1","price":"0.5","size":"1","side":"HOLD"}]})"},
          std::string_view{
              R"({"event_type":"price_change","price_changes":[{"price":"0.5","size":"1","side":"BUY"}]})"},
          std::string_view{R"({"event_type":"tick_size_change","asset_id":"1"})"},
          std::string_view{R"({"event_type":"book","asset_id":"1"} trailing)"},
      }) {
    EXPECT_EQ(status(bad), ParseStatus::Error) << "should reject: " << bad;
    EXPECT_STRNE(parser.last_error(), "") << bad;
  }
}

// Every truncated prefix of a valid frame must be handled without crashing
// or reading out of bounds (ASan would catch that in the debug build).
TEST_F(Fixture, TruncatedFramesNeverCrash) {
  for (std::string_view full : {kDocsBook, kDocsPriceChange, kDocsLastTrade, kDocsTickSize}) {
    for (std::size_t n = 0; n < full.size(); ++n) {
      std::string prefix(full.substr(0, n));
      auto s = parser.parse(prefix, frame);
      EXPECT_NE(s, ParseStatus::Pong);
    }
  }
}

// Real frames from Polymarket, cross-checked field by field against a second,
// independent JSON library (nlohmann). If both agree, the fast path is right.
TEST_F(Fixture, LiveFramesMatchIndependentParser) {
  std::ifstream in(std::string(PM_TEST_DATA_DIR) + "/live_frames.jsonl");
  ASSERT_TRUE(in) << "missing live_frames.jsonl";
  std::string line;
  int frames = 0;
  int books = 0;
  int changes = 0;
  while (std::getline(in, line)) {
    const auto rec = nlohmann::json::parse(line);
    const std::string raw = rec.at("raw").get<std::string>();
    ++frames;
    if (raw == "PONG") {
      EXPECT_EQ(status(raw), ParseStatus::Pong);
      continue;
    }
    ASSERT_EQ(status(raw), ParseStatus::Ok) << parser.last_error();
    const auto j = nlohmann::json::parse(raw);
    const auto items = j.is_array() ? j : nlohmann::json::array({j});
    ASSERT_EQ(frame.messages.size(), items.size());

    auto expect_price = [](const nlohmann::json& s, Price got) {
      Price want = -1;
      ASSERT_TRUE(parse_price(s.get<std::string>(), want));
      EXPECT_EQ(got, want);
    };
    auto expect_size = [](const nlohmann::json& s, Size got) {
      Size want = -1;
      ASSERT_TRUE(parse_size(s.get<std::string>(), want));
      EXPECT_EQ(got, want);
    };

    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& it = items[i];
      const Message& m = frame.messages[i];
      EXPECT_EQ(m.event_type, it.at("event_type").get<std::string>());
      EXPECT_EQ(m.market, it.at("market").get<std::string>());
      EXPECT_EQ(std::to_string(m.ts_ms), it.at("timestamp").get<std::string>());
      if (m.type == MsgType::Book) {
        ++books;
        EXPECT_EQ(m.asset_id, it.at("asset_id").get<std::string>());
        EXPECT_EQ(m.hash, it.at("hash").get<std::string>());
        expect_price(it.at("tick_size"), m.tick);
        auto bids = frame.bids(m);
        auto asks = frame.asks(m);
        ASSERT_EQ(bids.size(), it.at("bids").size());
        ASSERT_EQ(asks.size(), it.at("asks").size());
        for (std::size_t k = 0; k < bids.size(); ++k) {
          expect_price(it["bids"][k]["price"], bids[k].price);
          expect_size(it["bids"][k]["size"], bids[k].size);
        }
        for (std::size_t k = 0; k < asks.size(); ++k) {
          expect_price(it["asks"][k]["price"], asks[k].price);
          expect_size(it["asks"][k]["size"], asks[k].size);
        }
      } else if (m.type == MsgType::PriceChange) {
        auto pcs = frame.price_changes(m);
        ASSERT_EQ(pcs.size(), it.at("price_changes").size());
        for (std::size_t k = 0; k < pcs.size(); ++k) {
          ++changes;
          const auto& c = it["price_changes"][k];
          EXPECT_EQ(pcs[k].asset_id, c.at("asset_id").get<std::string>());
          EXPECT_EQ(pcs[k].hash, c.at("hash").get<std::string>());
          expect_price(c.at("price"), pcs[k].price);
          expect_size(c.at("size"), pcs[k].size);
          expect_price(c.at("best_bid"), pcs[k].best_bid);
          expect_price(c.at("best_ask"), pcs[k].best_ask);
          EXPECT_EQ(pcs[k].side == Side::Buy ? "BUY" : "SELL", c.at("side").get<std::string>());
        }
      }
    }
  }
  EXPECT_EQ(frames, 5);
  EXPECT_EQ(books, 4);
  EXPECT_GT(changes, 0);
}
