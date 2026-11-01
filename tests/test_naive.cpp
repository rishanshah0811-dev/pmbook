// The naive baseline must be *correct*, or the before/after comparison would
// be meaningless. These tests hold it to the same outputs as the fast path.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app/processor.hpp"
#include "app/summary_compare.hpp"
#include "book/map_book.hpp"
#include "naive/naive_parser.hpp"
#include "record/record_reader.hpp"
#include "replay/replayer.hpp"

using namespace pm;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> sample_files() {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(PM_SAMPLE_DIR)) {
    const auto n = e.path().filename().string();
    if (n.starts_with("pm_") && n.ends_with(".jsonl.gz")) out.push_back(e.path().string());
  }
  return out;
}

// Field-by-field comparison of two parse results.
void expect_same(const ParsedFrame& a, const ParsedFrame& b, const std::string& ctx) {
  ASSERT_EQ(a.messages.size(), b.messages.size()) << ctx;
  for (std::size_t i = 0; i < a.messages.size(); ++i) {
    const Message& x = a.messages[i];
    const Message& y = b.messages[i];
    EXPECT_EQ(x.type, y.type) << ctx;
    EXPECT_EQ(x.asset_id, y.asset_id) << ctx;
    EXPECT_EQ(x.market, y.market) << ctx;
    EXPECT_EQ(x.hash, y.hash) << ctx;
    EXPECT_EQ(x.ts_ms, y.ts_ms) << ctx;
    EXPECT_EQ(x.tick, y.tick) << ctx;
    EXPECT_EQ(x.price, y.price) << ctx;
    EXPECT_EQ(x.size, y.size) << ctx;
    EXPECT_EQ(x.side, y.side) << ctx;
    ASSERT_EQ(a.bids(x).size(), b.bids(y).size()) << ctx;
    ASSERT_EQ(a.asks(x).size(), b.asks(y).size()) << ctx;
    for (std::size_t k = 0; k < a.bids(x).size(); ++k) EXPECT_EQ(a.bids(x)[k], b.bids(y)[k]) << ctx;
    for (std::size_t k = 0; k < a.asks(x).size(); ++k) EXPECT_EQ(a.asks(x)[k], b.asks(y)[k]) << ctx;
    const auto pa = a.price_changes(x), pb = b.price_changes(y);
    ASSERT_EQ(pa.size(), pb.size()) << ctx;
    for (std::size_t k = 0; k < pa.size(); ++k) {
      EXPECT_EQ(pa[k].asset_id, pb[k].asset_id) << ctx;
      EXPECT_EQ(pa[k].price, pb[k].price) << ctx;
      EXPECT_EQ(pa[k].size, pb[k].size) << ctx;
      EXPECT_EQ(pa[k].side, pb[k].side) << ctx;
      EXPECT_EQ(pa[k].best_bid, pb[k].best_bid) << ctx;
      EXPECT_EQ(pa[k].best_ask, pb[k].best_ask) << ctx;
      EXPECT_EQ(pa[k].has_bbo, pb[k].has_bbo) << ctx;
    }
  }
}

}  // namespace

TEST(NaiveParser, MatchesFastParserOnEveryRecordedFrame) {
  Parser fast;
  NaiveParser naive;
  ParsedFrame a, b;
  RecordReader r;
  Record rec;
  int n = 0;
  for (const auto& f : sample_files()) {
    ASSERT_TRUE(r.open(f));
    while (r.next(rec)) {
      const auto sa = fast.parse(rec.raw, a);
      const auto sb = naive.parse(rec.raw, b);
      ASSERT_EQ(sa, sb) << "frame " << n;
      expect_same(a, b, "frame " + std::to_string(n));
      if (::testing::Test::HasFailure()) return;
      ++n;
    }
  }
  EXPECT_EQ(n, 1533);
}

TEST(NaiveParser, RejectsTheSameMalformedFrames) {
  NaiveParser naive;
  ParsedFrame out;
  for (
      std::string_view bad : {
          std::string_view{"not json"},
          std::string_view{"42"},
          std::string_view{R"({"event_type":"book"})"},
          std::string_view{
              R"({"event_type":"book","asset_id":"1","bids":[{"price":"1.5","size":"1"}]})"},
          std::string_view{R"({"event_type":"book","asset_id":"1","bids":[{"price":"0.5"}]})"},
          std::string_view{
              R"({"event_type":"book","asset_id":"1","bids":[{"price":0.5,"size":"1"}]})"},
          std::string_view{
              R"({"event_type":"price_change","price_changes":[{"asset_id":"1","price":"0.5","size":"1","side":"HOLD"}]})"},
          std::string_view{R"({"event_type":"tick_size_change","asset_id":"1"})"},
      }) {
    EXPECT_EQ(naive.parse(bad, out), ParseStatus::Error) << bad;
  }
  EXPECT_EQ(naive.parse("PONG", out), ParseStatus::Pong);
}

TEST(NaivePipeline, ReplayProducesTheSameBooksAsTheLiveRun) {
  FrameProcessor<MapBook, NaiveParser> proc({.stats_interval_s = 0, .log_resyncs = false});
  replay_files(sample_files(), ReplayOptions{},
               [&](std::string_view raw, std::int64_t ts) { proc.on_frame(raw, ts); });
  std::ifstream in(std::string(PM_SAMPLE_DIR) + "/live_summary.json");
  std::stringstream ss;
  ss << in.rdbuf();
  const auto diffs = compare_summaries(ss.str(), proc.summary());
  for (const auto& d : diffs) ADD_FAILURE() << d;
  EXPECT_TRUE(diffs.empty());
  EXPECT_EQ(proc.parse_errors(), 0u);
}
