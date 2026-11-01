#include "feed/feed_client.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <utility>

#include "core/clock.hpp"

namespace pm {

FeedClient::FeedClient(FeedConfig cfg, FrameHandler on_frame, StatusHandler on_status)
    : cfg_(std::move(cfg)),
      on_frame_(std::move(on_frame)),
      on_status_(std::move(on_status)),
      subscribe_msg_(subscribe_message(cfg_.assets)) {
  ix::initNetSystem();
}

FeedClient::~FeedClient() { stop(); }

std::string FeedClient::subscribe_message(const std::vector<std::string>& assets) {
  std::string out = R"({"assets_ids":[)";
  for (std::size_t i = 0; i < assets.size(); ++i) {
    const std::string& a = assets[i];
    if (a.empty() ||
        !std::all_of(a.begin(), a.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return "";
    }
    if (i) out += ',';
    out += '"';
    out += a;
    out += '"';
  }
  out += R"(],"type":"market"})";
  return out;
}

void FeedClient::start() {
  std::lock_guard lock(mu_);
  if (running_) return;
  running_ = true;
  supervisor_ = std::thread([this] { supervise(); });
}

void FeedClient::stop() {
  {
    std::lock_guard lock(mu_);
    if (!running_) return;
    running_ = false;
  }
  cv_.notify_all();
  if (supervisor_.joinable()) supervisor_.join();
}

void FeedClient::force_disconnect() {
  {
    std::lock_guard lock(mu_);
    kill_requested_ = true;
  }
  cv_.notify_all();
}

bool FeedClient::sleep_for(std::chrono::milliseconds d) {
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, d, [this] { return !running_; });
  return running_;
}

void FeedClient::supervise() {
  using namespace std::chrono;
  std::mt19937 rng(std::random_device{}());
  milliseconds backoff = cfg_.backoff_initial;

  while (true) {
    {
      std::lock_guard lock(mu_);
      if (!running_) break;
      closed_ = false;
      kill_requested_ = false;
      close_reason_.clear();
    }
    // A fresh socket object per attempt keeps the state machine simple: no
    // leftovers from the previous connection.
    ws_ = std::make_unique<ix::WebSocket>();
    ws_->setUrl(cfg_.url);
    ws_->disableAutomaticReconnection();  // we own the retry policy
    ws_->setPingInterval(0);              // Polymarket wants text PINGs, not protocol pings

    const auto connected_at = steady_clock::now();
    bool got_open = false;
    ws_->setOnMessageCallback([this, &got_open](const ix::WebSocketMessagePtr& msg) {
      switch (msg->type) {
        case ix::WebSocketMessageType::Message: {
          const std::int64_t now = steady_ns();  // timestamp FIRST, before any work
          last_rx_ns_.store(now, std::memory_order_relaxed);
          frames_.fetch_add(1, std::memory_order_relaxed);
          on_frame_(msg->str, now);
          break;
        }
        case ix::WebSocketMessageType::Open:
          got_open = true;
          last_rx_ns_.store(steady_ns(), std::memory_order_relaxed);
          connects_.fetch_add(1, std::memory_order_relaxed);
          ws_->sendText(subscribe_msg_);  // (re)subscribe on every new connection
          on_status_(FeedStatus::Connected, "subscribed");
          break;
        case ix::WebSocketMessageType::Close:
        case ix::WebSocketMessageType::Error: {
          {
            std::lock_guard lock(mu_);
            closed_ = true;
            if (close_reason_.empty()) {
              close_reason_ = msg->type == ix::WebSocketMessageType::Close
                                  ? "server closed: " + std::to_string(msg->closeInfo.code) + " " +
                                        msg->closeInfo.reason
                                  : "error: " + msg->errorInfo.reason;
            }
          }
          cv_.notify_all();
          break;
        }
        default:
          break;
      }
    });
    ws_->start();

    // Babysit the connection: heartbeat + staleness check + stop/kill requests.
    auto next_ping = steady_clock::now() + cfg_.ping_interval;
    while (true) {
      std::unique_lock lock(mu_);
      cv_.wait_until(lock, next_ping, [this] { return !running_ || closed_ || kill_requested_; });
      if (kill_requested_ && close_reason_.empty()) close_reason_ = "forced disconnect";
      if (!running_ || closed_ || kill_requested_) break;
      lock.unlock();

      const auto now = steady_clock::now();
      if (now >= next_ping) {
        if (ws_->getReadyState() == ix::ReadyState::Open) ws_->sendText("PING");
        next_ping = now + cfg_.ping_interval;
      }
      const auto last = last_rx_ns_.load(std::memory_order_relaxed);
      if (last != 0 &&
          steady_ns() - last > duration_cast<nanoseconds>(cfg_.stale_timeout).count()) {
        std::lock_guard relock(mu_);
        close_reason_ = "stale: no data for " + std::to_string(cfg_.stale_timeout.count()) + " ms";
        break;  // heard nothing for too long: treat as dead
      }
    }

    // stop() closes the socket and joins IX's thread, so after this line no
    // more callbacks can arrive.
    ws_->stop();
    ws_.reset();
    last_rx_ns_.store(0, std::memory_order_relaxed);
    if (got_open) disconnects_.fetch_add(1, std::memory_order_relaxed);

    {
      std::lock_guard lock(mu_);
      if (!running_) break;
    }

    // Exponential backoff with jitter: wait 0.5 s, 1 s, 2 s, ... up to the max,
    // plus up to 20% random extra so many clients don't retry in lockstep.
    if (got_open && steady_clock::now() - connected_at >= cfg_.stable_after) {
      backoff = cfg_.backoff_initial;
    }
    const auto jitter =
        milliseconds(std::uniform_int_distribution<long long>(0, backoff.count() / 5)(rng));
    std::string reason;
    {
      std::lock_guard lock(mu_);
      reason = close_reason_.empty() ? "connection lost" : close_reason_;
    }
    on_status_(FeedStatus::Reconnecting,
               (got_open ? "connection lost (" : "connect failed (") + reason + ")");
    if (!sleep_for(backoff + jitter)) break;
    backoff = std::min(backoff * 2, cfg_.backoff_max);
  }
}

}  // namespace pm
