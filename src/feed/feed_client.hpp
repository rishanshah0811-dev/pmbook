#pragma once
// WebSocket client for Polymarket's market channel.
//
// It only does networking: connect, subscribe, heartbeat, reconnect. Frames
// are handed to a callback together with the time they arrived, and the
// caller decides what to do with them. It knows nothing about order books.
//
// Threads:
//  * IXWebSocket runs one thread per connection; it calls on_frame from that
//    thread.
//  * Our supervisor thread owns the connection lifecycle: connect, send PING
//    every few seconds, notice a dead connection, wait with exponential
//    backoff, reconnect, resubscribe.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ix {
class WebSocket;
}

namespace pm {

struct FeedConfig {
  std::string url = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
  std::vector<std::string> assets;                  // token IDs to subscribe to
  std::chrono::milliseconds ping_interval{10'000};  // docs: send PING every 10 s
  std::chrono::milliseconds stale_timeout{30'000};  // no data this long = dead link
  std::chrono::milliseconds backoff_initial{500};
  std::chrono::milliseconds backoff_max{30'000};
  // A connection that stayed up this long resets the backoff to initial.
  std::chrono::milliseconds stable_after{10'000};
};

enum class FeedStatus : std::uint8_t { Connected, Reconnecting };

class FeedClient {
 public:
  // frame: raw text frame (only valid during the call); recv_ns: steady clock
  // timestamp taken as the very first thing when the frame was handed to us.
  using FrameHandler = std::function<void(std::string_view frame, std::int64_t recv_ns)>;
  using StatusHandler = std::function<void(FeedStatus, std::string_view detail)>;

  FeedClient(FeedConfig cfg, FrameHandler on_frame, StatusHandler on_status);
  ~FeedClient();
  FeedClient(const FeedClient&) = delete;
  FeedClient& operator=(const FeedClient&) = delete;

  void start();
  void stop();
  // Drops the current connection as if the network failed. Used to test that
  // reconnect works against the real server ("chaos testing").
  void force_disconnect();

  // Builds {"assets_ids":[...],"type":"market"}. Returns "" if any ID is not
  // all digits (token IDs are decimal numbers; refusing anything else means
  // we never send malformed JSON).
  static std::string subscribe_message(const std::vector<std::string>& assets);

  std::uint64_t connects() const noexcept { return connects_.load(); }
  std::uint64_t disconnects() const noexcept { return disconnects_.load(); }
  std::uint64_t frames() const noexcept { return frames_.load(); }

 private:
  void supervise();
  // Sleeps up to `d`, waking early if stop() is called. Returns false if stopping.
  bool sleep_for(std::chrono::milliseconds d);

  FeedConfig cfg_;
  FrameHandler on_frame_;
  StatusHandler on_status_;
  std::string subscribe_msg_;

  std::thread supervisor_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool running_ = false;               // guarded by mu_
  bool closed_ = false;                // current connection ended; guarded by mu_
  bool kill_requested_ = false;        // guarded by mu_
  std::string close_reason_;           // why the last connection ended; guarded by mu_
  std::unique_ptr<ix::WebSocket> ws_;  // only touched by the supervisor thread

  // Atomics: written by the IX thread, read by others. std::atomic makes
  // each read/write indivisible and visible across threads.
  std::atomic<std::int64_t> last_rx_ns_{0};
  std::atomic<std::uint64_t> connects_{0};
  std::atomic<std::uint64_t> disconnects_{0};
  std::atomic<std::uint64_t> frames_{0};
};

}  // namespace pm
