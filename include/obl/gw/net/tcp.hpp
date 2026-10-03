#pragma once

// Non-blocking TCP for the gateway (kernel sockets). TCP_NODELAY is always on: an order must not
// wait for Nagle. Optional SO_BUSY_POLL makes the kernel spin on the NIC queue on reads.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace obl::gw::net {

inline std::uint64_t wall_ns() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

inline bool make_addr(const std::string& ip, std::uint16_t port, sockaddr_in& a) {
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  return inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1;
}

inline void tune(int fd, int busy_poll_us) {
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (busy_poll_us > 0) setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof busy_poll_us);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

class TcpConn {
 public:
  enum class State { Closed, Connecting, Open };

  TcpConn() = default;
  TcpConn(const TcpConn&) = delete;
  TcpConn& operator=(const TcpConn&) = delete;
  ~TcpConn() { close(); }

  bool connect(const std::string& ip, std::uint16_t port, int busy_poll_us = 0) {
    close();
    sockaddr_in a;
    if (!make_addr(ip, port, a)) return false;
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    tune(fd_, busy_poll_us);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
      st_ = State::Open;
      return true;
    }
    if (errno != EINPROGRESS) {
      close();
      return false;
    }
    st_ = State::Connecting;
    return true;
  }

  void adopt(int fd, int busy_poll_us = 0) {
    close();
    fd_ = fd;
    tune(fd_, busy_poll_us);
    st_ = State::Open;
  }

  // Connecting -> Open or Closed.
  State poll_connect() {
    if (st_ != State::Connecting) return st_;
    sockaddr_in peer;
    socklen_t len = sizeof peer;
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&peer), &len) == 0) {
      st_ = State::Open;
    } else if (errno != ENOTCONN) {
      close();
    } else {
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err != 0 && err != EINPROGRESS) close();
    }
    return st_;
  }

  // > 0 bytes read, 0 nothing available, -1 closed by peer or error (connection closed).
  long read(std::uint8_t* buf, std::size_t cap) {
    if (st_ != State::Open) return -1;
    const ssize_t n = ::recv(fd_, buf, cap, 0);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    close();
    return -1;
  }

  // Sends everything or keeps the rest for flush(). False if the connection failed.
  bool write(const std::uint8_t* p, std::size_t n) {
    if (st_ != State::Open) return false;
    if (!backlog_.empty()) {
      backlog_.insert(backlog_.end(), p, p + n);
      return flush();
    }
    const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
    if (w == static_cast<ssize_t>(n)) return true;
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close();
      return false;
    }
    const std::size_t done = w > 0 ? static_cast<std::size_t>(w) : 0;
    backlog_.insert(backlog_.end(), p + done, p + n);
    return true;
  }

  bool flush() {
    while (!backlog_.empty() && st_ == State::Open) {
      const ssize_t w = ::send(fd_, backlog_.data(), backlog_.size(), MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        close();
        return false;
      }
      backlog_.erase(backlog_.begin(), backlog_.begin() + w);
    }
    return st_ == State::Open;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    st_ = State::Closed;
    backlog_.clear();
  }

  State state() const { return st_; }
  bool open() const { return st_ == State::Open; }
  bool has_backlog() const { return !backlog_.empty(); }
  int fd() const { return fd_; }

 private:
  int fd_ = -1;
  State st_ = State::Closed;
  std::vector<std::uint8_t> backlog_;
};

class TcpListener {
 public:
  ~TcpListener() { close(); }
  // port 0 = any free port; see port() afterwards.
  bool listen(const std::string& ip, std::uint16_t port) {
    sockaddr_in a;
    if (!make_addr(ip, port, a)) return false;
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) {
      close();
      return false;
    }
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    socklen_t len = sizeof a;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    return true;
  }
  // -1 if nothing pending.
  int accept() { return fd_ < 0 ? -1 : ::accept(fd_, nullptr, nullptr); }
  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }
  bool open() const { return fd_ >= 0; }
  std::uint16_t port() const { return port_; }

 private:
  int fd_ = -1;
  std::uint16_t port_ = 0;
};

}  // namespace obl::gw::net
