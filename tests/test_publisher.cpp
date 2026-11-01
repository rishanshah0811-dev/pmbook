#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/processor.hpp"
#include "publish/publisher.hpp"
#include "publish/tcp_sink.hpp"
#include "queue/frame_pipeline.hpp"

using namespace pm;
using json = nlohmann::json;

namespace {

const std::set<std::string> kBookKeys = {
    "v",          "type",      "asset_id", "seq",  "ts_exchange_ms",
    "ts_recv_ns", "ts_pub_ns", "tick",     "bids", "asks"};
const std::set<std::string> kTradeKeys = {
    "v", "type", "asset_id", "seq", "ts_exchange_ms", "ts_recv_ns", "price", "size", "side"};
const std::set<std::string> kStatusKeys = {"v", "type", "state", "detail", "ts_pub_ns"};

std::set<std::string> keys(const json& j) {
  std::set<std::string> k;
  for (auto it = j.begin(); it != j.end(); ++it) k.insert(it.key());
  return k;
}

std::vector<json> lines(const std::string& s) {
  std::vector<json> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) out.push_back(json::parse(line));
  return out;
}

constexpr std::string_view kSnapA =
    R"({"event_type":"book","asset_id":"111","timestamp":"1000","tick_size":"0.01",
        "bids":[{"price":"0.60","size":"1200"},{"price":"0.61","size":"500.5"},{"price":"0.59","size":"7"}],
        "asks":[{"price":"0.64","size":"900"},{"price":"0.63","size":"300"}]})";
constexpr std::string_view kSnapB =
    R"({"event_type":"book","asset_id":"222","timestamp":"1001","tick_size":"0.001","bids":[],
        "asks":[{"price":"0.999","size":"5"}]})";

struct Pub : ::testing::Test {
  FrameProcessor<FlatBook> proc{{.stats_interval_s = 0, .log_resyncs = false}};
  StringSink sink;
  Publisher pub{sink, 2};  // depth 2

  void feed(std::string_view frame, std::int64_t recv_ns = 5) {
    proc.on_frame(frame, recv_ns);
    pub.on_events(proc.events(), recv_ns);
  }
};

}  // namespace

TEST_F(Pub, BookMessageMatchesContractExactly) {
  feed(kSnapA, 123456789);
  auto out = lines(sink.out);
  ASSERT_EQ(out.size(), 1u);
  const json& m = out[0];
  EXPECT_EQ(keys(m), kBookKeys);
  EXPECT_EQ(m["v"], 1);
  EXPECT_EQ(m["type"], "book");
  EXPECT_EQ(m["asset_id"], "111");  // string, not number
  EXPECT_EQ(m["seq"], 1);
  EXPECT_EQ(m["ts_exchange_ms"], 1000);
  EXPECT_EQ(m["ts_recv_ns"], 123456789);
  EXPECT_GT(m["ts_pub_ns"].get<std::int64_t>(), 0);
  EXPECT_DOUBLE_EQ(m["tick"].get<double>(), 0.01);
  // Best first, truncated to depth 2, exact decimals.
  EXPECT_EQ(m["bids"], json::parse("[[0.61,500.5],[0.6,1200]]"));
  EXPECT_EQ(m["asks"], json::parse("[[0.63,300],[0.64,900]]"));
}

TEST_F(Pub, EmptySideIsEmptyArray) {
  feed(kSnapB);
  auto m = lines(sink.out).at(0);
  EXPECT_EQ(m["bids"], json::array());
  EXPECT_EQ(m["asks"], json::parse("[[0.999,5]]"));
  EXPECT_DOUBLE_EQ(m["tick"].get<double>(), 0.001);
}

TEST_F(Pub, SeqIsPerAssetAndSharedByBooksAndTrades) {
  feed(kSnapA);
  feed(kSnapB);
  feed(R"({"event_type":"price_change","timestamp":"2000","price_changes":[
    {"asset_id":"111","price":"0.62","size":"10","side":"BUY","best_bid":"0.62","best_ask":"0.63"}]})");
  feed(
      R"({"event_type":"last_trade_price","asset_id":"111","price":"0.63","size":"4","side":"BUY","timestamp":"2001"})");
  feed(
      R"({"event_type":"last_trade_price","asset_id":"222","price":"0.999","size":"1","side":"SELL","timestamp":"2002"})");
  auto out = lines(sink.out);
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ(out[0]["asset_id"], "111");
  EXPECT_EQ(out[0]["seq"], 1);
  EXPECT_EQ(out[1]["asset_id"], "222");
  EXPECT_EQ(out[1]["seq"], 1);
  EXPECT_EQ(out[2]["seq"], 2);  // 111 book after delta
  EXPECT_EQ(out[2]["bids"][0], json::parse("[0.62,10]"));
  EXPECT_EQ(keys(out[3]), kTradeKeys);
  EXPECT_EQ(out[3]["seq"], 3);  // 111 trade shares the counter
  EXPECT_EQ(out[3]["side"], "BUY");
  EXPECT_DOUBLE_EQ(out[3]["price"].get<double>(), 0.63);
  EXPECT_EQ(out[4]["seq"], 2);  // 222 trade
  EXPECT_EQ(out[4]["side"], "SELL");
}

TEST_F(Pub, ResyncBecomesStatusMessage) {
  feed(kSnapA);
  sink.out.clear();
  feed(R"({"event_type":"price_change","price_changes":[
    {"asset_id":"111","price":"0.59","size":"10","side":"BUY","best_bid":"0.62","best_ask":"0.63"}]})");
  auto out = lines(sink.out);
  ASSERT_EQ(out.size(), 1u);  // no book: the asset went stale
  EXPECT_EQ(keys(out[0]), kStatusKeys);
  EXPECT_EQ(out[0]["state"], "resync");
  EXPECT_EQ(out[0]["detail"], "asset 111: best bid/ask differs from exchange");
}

TEST_F(Pub, StatusDetailIsEscaped) {
  pub.publish_status("reconnecting", "server closed: 1006 \"abnormal\"\n\tline2");
  auto m = lines(sink.out).at(0);
  EXPECT_EQ(m["detail"], "server closed: 1006 \"abnormal\"\n\tline2");
}

TEST_F(Pub, MalformedFrameDoesNotRepublishPreviousEvents) {
  feed(kSnapA);
  feed("not json");
  EXPECT_EQ(lines(sink.out).size(), 1u);
}

TEST(PipelineStatus, StatusSlotsStayInOrderWithFrames) {
  std::vector<std::string> seen;
  FramePipeline p(
      8, WaitMode::Spin, [&](std::string_view raw, std::int64_t) { seen.emplace_back(raw); },
      [&](SlotKind k, std::string_view d) {
        seen.push_back((k == SlotKind::Connected ? "C:" : "R:") + std::string(d));
      });
  p.start();
  p.push("f1", 1);
  p.push_status(SlotKind::Reconnecting, "lost");
  p.push_status(SlotKind::Connected, "subscribed");
  p.push("f2", 2);
  p.stop();
  EXPECT_EQ(seen, (std::vector<std::string>{"f1", "R:lost", "C:subscribed", "f2"}));
}

// ---- TCP --------------------------------------------------------------------

namespace {

int connect_local(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
  return fd;
}

// Reads until `n` complete lines arrived or 2 s passed.
std::vector<json> read_lines(int fd, std::size_t n) {
  std::string buf;
  std::vector<json> out;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (out.size() < n && std::chrono::steady_clock::now() < end) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    char tmp[4096];
    const ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
    if (r <= 0) break;
    buf.append(tmp, static_cast<std::size_t>(r));
    std::size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      out.push_back(json::parse(buf.substr(0, nl)));
      buf.erase(0, nl + 1);
    }
  }
  return out;
}

template <class Pred>
bool wait_for(Pred p) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!p() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return p();
}

}  // namespace

TEST(TcpPublish, StreamsLinesAndWelcomesLateClientsWithLatestBooks) {
  TcpSink sink(0);  // any free port
  ASSERT_TRUE(sink.start()) << sink.error();
  FrameProcessor<FlatBook> proc({.stats_interval_s = 0, .log_resyncs = false});
  Publisher pub(sink, 10);
  auto feed = [&](std::string_view f) {
    proc.on_frame(f, 1);
    pub.on_events(proc.events(), 1);
  };

  const int early = connect_local(sink.port());
  ASSERT_TRUE(wait_for([&] { return sink.has_new_clients(); }));
  feed(kSnapA);
  feed(kSnapB);
  auto got = read_lines(early, 2);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0]["asset_id"], "111");
  EXPECT_EQ(got[1]["asset_id"], "222");

  // A client joining later first gets the latest book of every asset (same
  // seq), then live lines.
  const int late = connect_local(sink.port());
  ASSERT_TRUE(wait_for([&] { return sink.has_new_clients(); }));
  feed(
      R"({"event_type":"last_trade_price","asset_id":"111","price":"0.63","size":"4","side":"BUY"})");
  auto welcome = read_lines(late, 3);
  ASSERT_EQ(welcome.size(), 3u);
  std::set<std::string> welcomed{welcome[0]["asset_id"], welcome[1]["asset_id"]};
  EXPECT_EQ(welcomed, (std::set<std::string>{"111", "222"}));
  EXPECT_EQ(welcome[0]["seq"], 1);
  EXPECT_EQ(welcome[2]["type"], "trade");
  EXPECT_EQ(welcome[2]["seq"], 2);
  EXPECT_EQ(read_lines(early, 1).at(0)["type"], "trade");
  EXPECT_EQ(sink.client_count(), 2u);
  ::close(early);
  ::close(late);
  sink.stop();
}

TEST(TcpPublish, ClientThatDisappearsIsDroppedNotFatal) {
  TcpSink sink(0);
  ASSERT_TRUE(sink.start());
  const int fd = connect_local(sink.port());
  ASSERT_TRUE(wait_for([&] { return sink.has_new_clients(); }));
  sink.adopt_new_clients({});
  ASSERT_EQ(sink.client_count(), 1u);
  ::close(fd);
  const std::string line(1000, 'x');
  for (int i = 0; i < 2000 && sink.client_count() == 1; ++i) {
    sink.write(line);  // must not raise SIGPIPE
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  EXPECT_EQ(sink.client_count(), 0u);
  EXPECT_EQ(sink.dropped_clients(), 1u);
  sink.stop();
}

TEST(TcpPublish, PortInUseIsReported) {
  TcpSink a(0);
  ASSERT_TRUE(a.start());
  TcpSink b(a.port());
  EXPECT_FALSE(b.start());
  EXPECT_NE(b.error().find(std::to_string(a.port())), std::string::npos);
}
