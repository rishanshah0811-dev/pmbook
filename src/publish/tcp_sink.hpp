#pragma once
// Streams published lines to TCP clients on 127.0.0.1:<port>.
//
// An acceptor thread waits for connections and parks new sockets in a
// mutex-protected list. The processing thread (the only one that writes)
// adopts them at the start of a publish, sends each one the latest book per
// asset, then broadcasts every line to all clients.
//
// Sockets are non-blocking: a slow client never stalls the engine. Unsent
// bytes are buffered per client; past 8 MB the client is dropped.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "publish/sink.hpp"

namespace pm {

class TcpSink final : public Sink {
 public:
  explicit TcpSink(std::uint16_t port);
  ~TcpSink() override;
  TcpSink(const TcpSink&) = delete;
  TcpSink& operator=(const TcpSink&) = delete;

  // Binds and starts accepting. Returns false (with error()) if the port is taken.
  bool start();
  void stop();

  void write(std::string_view data) override;
  void flush() override;
  bool has_new_clients() const noexcept override {
    return pending_flag_.load(std::memory_order_relaxed);
  }
  void adopt_new_clients(const std::vector<std::string_view>& welcome_lines) override;

  std::uint16_t port() const noexcept { return port_; }
  const std::string& error() const noexcept { return error_; }
  std::size_t client_count() const noexcept { return clients_.size(); }
  std::uint64_t dropped_clients() const noexcept { return dropped_; }

  static constexpr std::size_t kMaxBuffered = 8u << 20;

 private:
  struct Client {
    int fd;
    std::string pending;  // bytes the kernel wouldn't take yet
  };
  void accept_loop();
  // Tries to send buffered + new bytes. Returns false if the client must go.
  bool send_to(Client& c, std::string_view data);

  std::uint16_t port_;
  int listen_fd_ = -1;
  std::string error_;
  std::thread acceptor_;
  std::atomic<bool> running_{false};

  std::mutex pending_mu_;
  std::vector<int> pending_fds_;  // accepted, not yet adopted
  std::atomic<bool> pending_flag_{false};

  std::vector<Client> clients_;  // processing thread only
  std::uint64_t dropped_ = 0;
};

}  // namespace pm
