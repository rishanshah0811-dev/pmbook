#include "publish/tcp_sink.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace pm {

namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // Linux: don't raise SIGPIPE
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set per socket instead
#endif
}  // namespace

TcpSink::TcpSink(std::uint16_t port) : port_(port) {}

TcpSink::~TcpSink() { stop(); }

bool TcpSink::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    error_ = std::strerror(errno);
    return false;
  }
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only, never exposed
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    error_ = "127.0.0.1:" + std::to_string(port_) + ": " + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (port_ == 0) {  // tests ask the OS for any free port
    socklen_t len = sizeof addr;
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
  }
  running_.store(true);
  acceptor_ = std::thread([this] { accept_loop(); });
  return true;
}

void TcpSink::stop() {
  if (!running_.exchange(false)) return;
  if (acceptor_.joinable()) acceptor_.join();
  if (listen_fd_ >= 0) ::close(listen_fd_);
  listen_fd_ = -1;
  for (auto& c : clients_) ::close(c.fd);
  clients_.clear();
  std::lock_guard lock(pending_mu_);
  for (int fd : pending_fds_) ::close(fd);
  pending_fds_.clear();
}

void TcpSink::accept_loop() {
  while (running_.load()) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) continue;  // 100 ms timeout lets stop() finish promptly
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // don't batch small lines
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    std::lock_guard lock(pending_mu_);
    pending_fds_.push_back(fd);
    pending_flag_.store(true, std::memory_order_relaxed);
  }
}

void TcpSink::adopt_new_clients(const std::vector<std::string_view>& welcome_lines) {
  std::vector<int> fds;
  {
    std::lock_guard lock(pending_mu_);
    fds.swap(pending_fds_);
    pending_flag_.store(false, std::memory_order_relaxed);
  }
  for (int fd : fds) {
    Client c{fd, {}};
    bool ok = true;
    for (auto line : welcome_lines) ok = ok && send_to(c, line);
    if (ok) {
      clients_.push_back(std::move(c));
      std::fprintf(stderr, "[publish] tcp client connected (%zu total)\n", clients_.size());
    } else {
      ::close(fd);
    }
  }
}

bool TcpSink::send_to(Client& c, std::string_view data) {
  if (!c.pending.empty()) {
    c.pending.append(data);
    data = c.pending;
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(c.fd, data.data() + sent, data.size() - sent, kSendFlags);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;  // kernel buffer full: keep the rest for later
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;  // client went away
    }
  }
  std::string rest(data.substr(sent));
  c.pending.swap(rest);
  return c.pending.size() <= kMaxBuffered;
}

void TcpSink::write(std::string_view data) {
  for (std::size_t i = 0; i < clients_.size();) {
    if (send_to(clients_[i], data)) {
      ++i;
    } else {
      ::close(clients_[i].fd);
      clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
      ++dropped_;
      std::fprintf(stderr, "[publish] tcp client disconnected or too slow (%zu left)\n",
                   clients_.size());
    }
  }
}

void TcpSink::flush() {
  // Retry anything buffered for slow clients.
  for (auto& c : clients_) {
    if (!c.pending.empty()) send_to(c, {});
  }
}

}  // namespace pm
