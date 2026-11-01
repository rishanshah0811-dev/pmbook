#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "book/book_manager.hpp"
#include "book/map_book.hpp"
#include "parse/parser.hpp"

using namespace pm;

namespace {

struct Mgr : ::testing::Test {
  Parser parser;
  ParsedFrame frame;
  BookManager<FlatBook> mgr;
  std::vector<BookManager<FlatBook>::Event> events;

  void feed(std::string_view json) {
    ASSERT_EQ(parser.parse(json, frame), ParseStatus::Ok) << parser.last_error();
    events.clear();
    mgr.apply(frame, events);
  }
  int count(EventKind k) const {
    int n = 0;
    for (const auto& e : events) n += e.kind == k;
    return n;
  }
  const FlatBook& book(std::string_view id) { return *mgr.find(id)->book; }
};

constexpr std::string_view kSnapA =
    R"({"event_type":"book","asset_id":"A","timestamp":"1000","tick_size":"0.01",
        "bids":[{"price":"0.60","size":"1200"},{"price":"0.61","size":"500"}],
        "asks":[{"price":"0.64","size":"900"},{"price":"0.63","size":"300"}]})";

std::string change(std::string_view asset, std::string_view price, std::string_view size,
                   std::string_view side, std::string_view bb, std::string_view ba) {
  return std::string(R"({"event_type":"price_change","timestamp":"2000","price_changes":[)") +
         R"({"asset_id":")" + std::string(asset) + R"(","price":")" + std::string(price) +
         R"(","size":")" + std::string(size) + R"(","side":")" + std::string(side) +
         R"(","best_bid":")" + std::string(bb) + R"(","best_ask":")" + std::string(ba) + R"("}]})";
}

}  // namespace

TEST_F(Mgr, SnapshotThenDeltas) {
  feed(kSnapA);
  EXPECT_EQ(count(EventKind::BookUpdated), 1);
  EXPECT_EQ(mgr.find("A")->state, AssetState::Live);
  EXPECT_EQ(mgr.find("A")->tick, 100);
  EXPECT_EQ(book("A").best_bid(), 6100);

  feed(change("A", "0.62", "400", "BUY", "0.62", "0.63"));
  EXPECT_EQ(count(EventKind::BookUpdated), 1);
  EXPECT_EQ(count(EventKind::ResyncNeeded), 0);
  EXPECT_EQ(book("A").best_bid(), 6200);

  feed(change("A", "0.63", "0", "SELL", "0.62", "0.64"));
  EXPECT_EQ(book("A").best_ask(), 6400);
  EXPECT_EQ(mgr.find("A")->last_ts_ms, 2000);
}

TEST_F(Mgr, DeltaBeforeSnapshotIsIgnored) {
  feed(change("A", "0.62", "400", "BUY", "0.62", "0.63"));
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(mgr.find("A"), nullptr);
  EXPECT_EQ(mgr.stats().ignored_deltas, 1u);
}

TEST_F(Mgr, BestBidAskMismatchTriggersResyncUntilNextSnapshot) {
  feed(kSnapA);
  // Exchange says best bid is 0.62, but we never saw an order there:
  // we must have missed a message.
  feed(change("A", "0.59", "10", "BUY", "0.62", "0.63"));
  ASSERT_EQ(count(EventKind::ResyncNeeded), 1);
  EXPECT_STREQ(events[0].reason, "best bid/ask differs from exchange");
  EXPECT_EQ(mgr.find("A")->state, AssetState::NeedsResync);

  feed(change("A", "0.58", "10", "BUY", "0.62", "0.63"));  // ignored while resyncing
  EXPECT_TRUE(events.empty());

  feed(kSnapA);  // fresh snapshot heals it
  EXPECT_EQ(mgr.find("A")->state, AssetState::Live);
  EXPECT_EQ(book("A").size_at(Side::Buy, 5900), 0);
}

TEST_F(Mgr, CrossedAfterDeltaTriggersResync) {
  feed(kSnapA);
  // A bid at 0.65 crosses our best ask 0.63. (No bbo fields, so only the
  // crossed check can catch it.)
  feed(
      R"({"event_type":"price_change","price_changes":[{"asset_id":"A","price":"0.65","size":"1","side":"BUY"}]})");
  ASSERT_EQ(count(EventKind::ResyncNeeded), 1);
  EXPECT_STREQ(events[0].reason, "crossed book after delta");
}

TEST_F(Mgr, OneUpdateEventPerAssetPerMessage) {
  feed(kSnapA);
  feed(R"({"event_type":"price_change","price_changes":[
    {"asset_id":"A","price":"0.59","size":"5","side":"BUY","best_bid":"0.61","best_ask":"0.63"},
    {"asset_id":"A","price":"0.58","size":"5","side":"BUY","best_bid":"0.61","best_ask":"0.63"}]})");
  EXPECT_EQ(count(EventKind::BookUpdated), 1);
  EXPECT_EQ(book("A").level_count(Side::Buy), 4u);
}

TEST_F(Mgr, TradesAndTickSizeChanges) {
  feed(kSnapA);
  feed(
      R"({"event_type":"last_trade_price","asset_id":"A","price":"0.61","size":"10","side":"SELL","timestamp":"3000"})");
  ASSERT_EQ(count(EventKind::Trade), 1);
  EXPECT_EQ(events[0].msg->price, 6100);
  feed(
      R"({"event_type":"tick_size_change","asset_id":"A","old_tick_size":"0.01","new_tick_size":"0.001"})");
  EXPECT_EQ(mgr.find("A")->tick, 10);
  EXPECT_EQ(book("A").best_bid(), 6100);  // book untouched by tick change
}

TEST_F(Mgr, InvalidateAllAfterReconnect) {
  feed(kSnapA);
  mgr.invalidate_all();
  EXPECT_EQ(mgr.find("A")->state, AssetState::NeedsResync);
}

// Replays a real 20-second capture. Every price_change carries the exchange's
// best bid/ask, so this checks our book against Polymarket's own numbers on
// every single update. Both book types must agree with each other too.
TEST(LiveSession, RealCaptureStaysConsistentWithExchange) {
  std::ifstream in(std::string(PM_TEST_DATA_DIR) + "/live_session_20s.jsonl");
  ASSERT_TRUE(in);
  Parser parser;
  ParsedFrame frame;
  BookManager<FlatBook> flat;
  BookManager<MapBook> ref;
  std::vector<BookManager<FlatBook>::Event> ev1;
  std::vector<BookManager<MapBook>::Event> ev2;
  std::string line;
  int checked = 0;
  while (std::getline(in, line)) {
    const auto raw = nlohmann::json::parse(line).at("raw").get<std::string>();
    if (parser.parse(raw, frame) != ParseStatus::Ok) continue;
    flat.apply(frame, ev1);
    ref.apply(frame, ev2);
    for (const auto& m : frame.messages) {
      for (const auto& pc : frame.price_changes(m)) checked += pc.has_bbo;
    }
  }
  for (const auto& e : ev1) {
    EXPECT_NE(e.kind, EventKind::ResyncNeeded) << e.asset->asset_id << ": " << e.reason;
  }
  EXPECT_EQ(flat.stats().bbo_mismatches, 0u);
  EXPECT_EQ(flat.stats().crossed, 0u);
  EXPECT_EQ(flat.stats().ignored_deltas, 0u);
  EXPECT_GT(checked, 0);
  ASSERT_EQ(flat.assets().size(), 4u);
  for (const auto& [id, a] : flat.assets()) {
    EXPECT_EQ(a.state, AssetState::Live) << id;
    EXPECT_EQ(state_hash(*a.book), state_hash(*ref.find(id)->book)) << id;
  }
  std::cout << "  checked best bid/ask against exchange on " << checked << " live updates\n";
}
