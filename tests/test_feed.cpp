// Feed client tests against a fake exchange running on localhost, so the
// reconnect logic is tested offline and deterministically.
#include <gtest/gtest.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "feed/feed_client.hpp"

using namespace pm;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 5s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return pred();
}

constexpr const char* kBook =
    R"([{"event_type":"book","asset_id":"123","bids":[{"price":"0.5","size":"1"}],"asks":[]}])";

// Pretends to be Polymarket: answers PING with PONG and sends a book after
// each subscribe.
class FakeExchange {
 public:
  FakeExchange() : port_(ix::getFreePort()), server_(port_, "127.0.0.1") {
    server_.disablePerMessageDeflate();
    server_.setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState>,
                                              ix::WebSocket& ws,
                                              const ix::WebSocketMessagePtr& msg) {
      if (msg->type != ix::WebSocketMessageType::Message) return;
      if (msg->str == "PING") {
        pings.fetch_add(1);
        if (reply_pong.load()) ws.sendText("PONG");
        return;
      }
      {
        std::lock_guard lock(mu_);
        subscribes_.push_back(msg->str);
      }
      if (send_book.load()) ws.sendText(kBook);
    });
    auto [ok, err] = server_.listen();
    EXPECT_TRUE(ok) << err;
    server_.start();
  }
  ~FakeExchange() { server_.stop(); }

  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/ws/market"; }
  void drop_all_clients() {
    for (const auto& c : server_.getClients()) c->close();
  }
  std::vector<std::string> subscribes() {
    std::lock_guard lock(mu_);
    return subscribes_;
  }
  std::size_t subscribe_count() { return subscribes().size(); }

  std::atomic<int> pings{0};
  std::atomic<bool> reply_pong{true};
  std::atomic<bool> send_book{true};

 private:
  int port_;
  ix::WebSocketServer server_;
  std::mutex mu_;
  std::vector<std::string> subscribes_;
};

FeedConfig fast_config(const std::string& url) {
  FeedConfig c;
  c.url = url;
  c.assets = {"123", "456"};
  c.ping_interval = 100ms;
  c.stale_timeout = 2s;
  c.backoff_initial = 50ms;
  c.backoff_max = 400ms;
  c.stable_after = 10s;
  return c;
}

// Collects callbacks from the client's threads.
struct Sink {
  std::mutex mu;
  std::vector<std::string> frames;
  std::vector<std::pair<FeedStatus, std::chrono::steady_clock::time_point>> statuses;
  std::vector<std::string> details;
  std::int64_t last_recv_ns = 0;

  FeedClient::FrameHandler frame_handler() {
    return [this](std::string_view f, std::int64_t ns) {
      std::lock_guard lock(mu);
      frames.emplace_back(f);
      last_recv_ns = ns;
    };
  }
  FeedClient::StatusHandler status_handler() {
    return [this](FeedStatus s, std::string_view detail) {
      std::lock_guard lock(mu);
      statuses.emplace_back(s, std::chrono::steady_clock::now());
      details.emplace_back(detail);
    };
  }
  std::size_t frame_count() {
    std::lock_guard lock(mu);
    return frames.size();
  }
  bool any_detail_contains(std::string_view needle) {
    std::lock_guard lock(mu);
    for (const auto& d : details) {
      if (d.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  int count(FeedStatus s) {
    std::lock_guard lock(mu);
    int n = 0;
    for (auto& [st, t] : statuses) n += st == s;
    return n;
  }
};

}  // namespace

TEST(FeedClient, SubscribeMessageFormat) {
  EXPECT_EQ(FeedClient::subscribe_message({"123", "456"}),
            R"({"assets_ids":["123","456"],"type":"market"})");
  EXPECT_EQ(FeedClient::subscribe_message({"12\"3"}), "");  // would break the JSON
  EXPECT_EQ(FeedClient::subscribe_message({""}), "");
}

TEST(FeedClient, SubscribesReceivesFramesAndSendsHeartbeat) {
  FakeExchange ex;
  Sink sink;
  FeedClient client(fast_config(ex.url()), sink.frame_handler(), sink.status_handler());
  client.start();
  ASSERT_TRUE(wait_until([&] { return sink.frame_count() >= 1 && ex.pings.load() >= 3; }));
  EXPECT_EQ(ex.subscribes().at(0), R"({"assets_ids":["123","456"],"type":"market"})");
  {
    std::lock_guard lock(sink.mu);
    EXPECT_EQ(sink.frames[0], kBook);
    EXPECT_GT(sink.last_recv_ns, 0);
  }
  EXPECT_EQ(client.connects(), 1u);
  client.stop();
}

TEST(FeedClient, ReconnectsAndResubscribesWhenServerDropsUs) {
  FakeExchange ex;
  Sink sink;
  FeedClient client(fast_config(ex.url()), sink.frame_handler(), sink.status_handler());
  client.start();
  ASSERT_TRUE(wait_until([&] { return ex.subscribe_count() == 1; }));
  ex.drop_all_clients();
  ASSERT_TRUE(wait_until([&] { return ex.subscribe_count() == 2 && client.connects() == 2; }));
  const auto subs = ex.subscribes();
  EXPECT_EQ(subs[0], subs[1]);  // same subscription after reconnect
  EXPECT_GE(sink.count(FeedStatus::Reconnecting), 1);
  EXPECT_TRUE(sink.any_detail_contains("server closed"));
  EXPECT_EQ(sink.count(FeedStatus::Connected), 2);
  ASSERT_TRUE(wait_until([&] { return sink.frame_count() >= 2; }));  // fresh snapshot arrived
  client.stop();
}

TEST(FeedClient, ForceDisconnectReconnects) {
  FakeExchange ex;
  Sink sink;
  FeedClient client(fast_config(ex.url()), sink.frame_handler(), sink.status_handler());
  client.start();
  ASSERT_TRUE(wait_until([&] { return client.connects() == 1; }));
  client.force_disconnect();
  ASSERT_TRUE(wait_until([&] { return client.connects() == 2; }));
  EXPECT_EQ(client.disconnects(), 1u);
  EXPECT_TRUE(sink.any_detail_contains("forced disconnect"));
  client.stop();
}

TEST(FeedClient, DetectsSilentDeadConnection) {
  FakeExchange ex;
  ex.reply_pong = false;  // server goes quiet: no PONG, no data
  ex.send_book = false;
  Sink sink;
  auto cfg = fast_config(ex.url());
  cfg.stale_timeout = 300ms;
  FeedClient client(cfg, sink.frame_handler(), sink.status_handler());
  client.start();
  ASSERT_TRUE(wait_until([&] { return client.connects() >= 2; }));
  EXPECT_GE(client.disconnects(), 1u);
  EXPECT_TRUE(sink.any_detail_contains("stale: no data for 300 ms"));
  client.stop();
}

TEST(FeedClient, BacksOffExponentiallyWhileServerIsDown) {
  // Nothing listens on this port, so every attempt fails.
  const std::string url = "ws://127.0.0.1:" + std::to_string(ix::getFreePort()) + "/";
  Sink sink;
  FeedClient client(fast_config(url), sink.frame_handler(), sink.status_handler());
  client.start();
  ASSERT_TRUE(wait_until([&] { return sink.count(FeedStatus::Reconnecting) >= 5; }, 6s));
  client.stop();

  std::lock_guard lock(sink.mu);
  std::vector<double> gaps_ms;
  for (std::size_t i = 1; i < sink.statuses.size(); ++i) {
    gaps_ms.push_back(std::chrono::duration<double, std::milli>(sink.statuses[i].second -
                                                                sink.statuses[i - 1].second)
                          .count());
  }
  ASSERT_GE(gaps_ms.size(), 4u);
  std::cout << "  retry gaps (ms):";
  for (double g : gaps_ms) std::cout << ' ' << static_cast<int>(g);
  std::cout << '\n';
  // Expected waits: ~50, ~100, ~200, ~400 ms (+ up to 20% jitter).
  EXPECT_GE(gaps_ms[0], 45.0);
  EXPECT_GT(gaps_ms[2], gaps_ms[0] * 2.5);
  EXPECT_LT(gaps_ms[3], 400 * 1.2 + 200);  // capped at backoff_max (+ slack for slow CI)
  EXPECT_EQ(client.connects(), 0u);
}
