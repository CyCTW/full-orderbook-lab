#pragma once

// Connects an OcgcGateway to OCG-C over TCP and keeps it connected (§3.3, §3.4, §3.8, §4.1):
//
//   Lookup     try the Lookup Service endpoints in order (primary site primary, primary site
//              mirror, backup site primary, backup site mirror, then round again); 5 s pause
//              after an unreachable or rejecting endpoint. The response names the primary and
//              mirror trading service.
//   Connect    primary first, then mirror; both failing -> Lookup again.
//   Online     bytes in -> gateway; gateway output -> socket (TCP_NODELAY, backlog on EAGAIN).
//   Lost       wait 10 s, then primary, mirror, Lookup as above.
//
// step(now) does all of it without blocking; call it in the event loop (busy poll) together
// with whatever else the thread does. The gateway's Transport is ConnTransport, which points at
// the current trading connection.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "obl/gw/net/tcp.hpp"
#include "obl/gw/ocgc/messages.hpp"

namespace obl::gw::ocgc {

struct Endpoint {
  std::string ip;
  std::uint16_t port = 0;
};

struct ConnectorConfig {
  std::vector<Endpoint> lookup;  // empty: connect to primary / secondary directly
  Endpoint primary, secondary;
  std::string comp_id;
  std::uint64_t lookup_retry_ns = 5'000'000'000;    // §3.4
  std::uint64_t reconnect_delay_ns = 10'000'000'000;  // §4.1
  std::uint64_t connect_timeout_ns = 5'000'000'000;
  int busy_poll_us = 0;
};

// The gateway's Transport: writes to whichever trading connection is current.
struct ConnTransport {
  net::TcpConn* conn = nullptr;
  bool close_requested = false;
  void send(const std::uint8_t* p, std::size_t n) {
    if (conn) conn->write(p, n);
  }
  void close() { close_requested = true; }
};

template <class Gateway>
class Connector {
 public:
  enum class State { Idle, LookupConnecting, LookupWaiting, Connecting, Online, Waiting };

  Connector(ConnectorConfig cfg, Gateway& gw, ConnTransport& tx) : cfg_(std::move(cfg)), gw_(gw), tx_(tx) {}

  void start(std::uint64_t now) {
    stopped_ = false;
    if (cfg_.lookup.empty()) begin_connect(now, 0);
    else begin_lookup(now);
  }
  // Stop reconnecting (end of day after Logout).
  void stop() { stopped_ = true; }

  State state() const { return st_; }
  const Endpoint& current_primary() const { return primary_; }
  int lookup_index() const { return lookup_i_; }
  int target() const { return target_; }  // 0 primary, 1 mirror
  std::uint32_t connects() const { return connects_; }

  void step(std::uint64_t now) {
    switch (st_) {
      case State::Idle:
        break;
      case State::Waiting:
        if (now >= wake_ && !stopped_) next_after_wait_(now);
        break;
      case State::LookupConnecting:
        if (lookup_.poll_connect() == net::TcpConn::State::Open) {
          std::uint8_t b[128];
          lookup_.write(b, encode_lookup_request(b, cfg_.comp_id));
          rx_.clear();
          st_ = State::LookupWaiting;
        } else if (!lookup_.open() && lookup_.state() == net::TcpConn::State::Closed) {
          lookup_failed(now);
        } else if (now - since_ > cfg_.connect_timeout_ns) {
          lookup_failed(now);
        }
        break;
      case State::LookupWaiting:
        read_lookup(now);
        break;
      case State::Connecting: {
        const auto s = conn_.poll_connect();
        if (s == net::TcpConn::State::Open) {
          ++connects_;
          tx_.conn = &conn_;
          tx_.close_requested = false;
          st_ = State::Online;
          gw_.on_connected(now);
          conn_.flush();
        } else if (s == net::TcpConn::State::Closed || now - since_ > cfg_.connect_timeout_ns) {
          connect_failed(now);
        }
        break;
      }
      case State::Online:
        online(now);
        break;
    }
    gw_.poll(now);
    if (st_ == State::Online) conn_.flush();
  }

 private:
  void begin_lookup(std::uint64_t now) {
    since_ = now;
    const Endpoint& e = cfg_.lookup[static_cast<std::size_t>(lookup_i_)];
    if (!lookup_.connect(e.ip, e.port)) return lookup_failed(now);
    st_ = State::LookupConnecting;
  }
  void lookup_failed(std::uint64_t now) {
    lookup_.close();
    lookup_i_ = (lookup_i_ + 1) % static_cast<int>(cfg_.lookup.size());
    wait(now, cfg_.lookup_retry_ns, [this](std::uint64_t t) { begin_lookup(t); });
  }
  void read_lookup(std::uint64_t now) {
    std::uint8_t b[512];
    const long n = lookup_.read(b, sizeof b);
    if (n > 0) rx_.insert(rx_.end(), b, b + n);
    if (rx_.size() >= 3) {
      const std::size_t len = load_le<std::uint16_t>(rx_.data() + hdr::kLength);
      if (rx_.size() >= len) {
        LookupResult r;
        const bool ok = valid_frame(rx_.data(), len) && read_header(rx_.data()).type == MsgType::LookupResponse &&
                        decode_lookup_response(rx_.data(), len, r) && r.accepted;
        if (!ok) return lookup_failed(now);
        primary_ = {std::string(r.primary_ip.view()), r.primary_port};
        secondary_ = {std::string(r.secondary_ip.view()), r.secondary_port};
        lookup_.close();
        return begin_connect(now, 0);
      }
    }
    if (n < 0 || now - since_ > cfg_.connect_timeout_ns) lookup_failed(now);
  }

  void begin_connect(std::uint64_t now, int target) {
    if (cfg_.lookup.empty()) {
      primary_ = cfg_.primary;
      secondary_ = cfg_.secondary;
    }
    target_ = target;
    since_ = now;
    const Endpoint& e = target == 0 ? primary_ : secondary_;
    if (e.port == 0 || !conn_.connect(e.ip, e.port, cfg_.busy_poll_us)) return connect_failed(now);
    st_ = State::Connecting;
  }
  void connect_failed(std::uint64_t now) {
    conn_.close();
    if (target_ == 0) return begin_connect(now, 1);  // primary, then mirror
    if (!cfg_.lookup.empty()) return begin_lookup(now);
    wait(now, cfg_.reconnect_delay_ns, [this](std::uint64_t t) { begin_connect(t, 0); });
  }

  void online(std::uint64_t now) {
    std::uint8_t b[16384];
    for (int i = 0; i < 8; ++i) {  // bounded: don't starve the rest of the loop
      const long n = conn_.read(b, sizeof b);
      if (n > 0) {
        gw_.on_bytes(b, static_cast<std::size_t>(n), now);
        if (tx_.close_requested) break;
        continue;
      }
      if (n < 0) return lost(now);
      break;
    }
    if (tx_.close_requested || !conn_.open()) lost(now);
  }

  void lost(std::uint64_t now) {
    conn_.flush();
    conn_.close();
    tx_.conn = nullptr;
    tx_.close_requested = false;
    gw_.on_disconnected(now);
    if (stopped_) {
      st_ = State::Idle;
      return;
    }
    wait(now, cfg_.reconnect_delay_ns, [this](std::uint64_t t) { begin_connect(t, 0); });
  }

  template <class F>
  void wait(std::uint64_t now, std::uint64_t delay, F next) {
    st_ = State::Waiting;
    wake_ = now + delay;
    next_after_wait_ = next;
  }

  ConnectorConfig cfg_;
  Gateway& gw_;
  ConnTransport& tx_;
  State st_ = State::Idle;
  bool stopped_ = false;
  net::TcpConn lookup_, conn_;
  std::vector<std::uint8_t> rx_;
  Endpoint primary_, secondary_;
  int lookup_i_ = 0;
  int target_ = 0;
  std::uint64_t since_ = 0, wake_ = 0;
  std::uint32_t connects_ = 0;
  std::function<void(std::uint64_t)> next_after_wait_;
};

}  // namespace obl::gw::ocgc
