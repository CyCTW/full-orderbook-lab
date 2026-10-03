#pragma once

// The exchange simulator over real TCP: a Lookup Service port and a primary and mirror trading
// port that share one Exchange (so a client failing over keeps its sequence numbers and orders,
// as with the real OCG-C primary / mirror pair). Non-blocking; call step(now) in a loop.

#include <memory>
#include <string>
#include <vector>

#include "obl/gw/net/tcp.hpp"
#include "obl/gw/ocgc/sim/exchange.hpp"

namespace obl::gw::ocgc::sim {

class Server {
 public:
  explicit Server(Exchange& ex, std::string ip = "127.0.0.1") : ex_(ex), ip_(std::move(ip)) {}

  bool start() {
    return lookup_.listen(ip_, 0) && trading_[0].listen(ip_, 0) && trading_[1].listen(ip_, 0);
  }
  std::uint16_t lookup_port() const { return lookup_.port(); }
  std::uint16_t primary_port() const { return trading_[0].port(); }
  std::uint16_t mirror_port() const { return trading_[1].port(); }

  // Take the primary trading service down (connections dropped, no new ones accepted).
  void fail_primary() {
    trading_[0].close();
    for (auto& c : clients_)
      if (c->which == 0 && c->conn.open()) drop(*c);
  }
  void reject_lookups(bool on) { reject_lookups_ = on; }

  void step(std::uint64_t now) {
    if (int fd = lookup_.accept(); fd >= 0) {
      auto c = std::make_unique<Client>();
      c->conn.adopt(fd);
      c->lookup = true;
      clients_.push_back(std::move(c));
    }
    for (int w = 0; w < 2; ++w) {
      if (!trading_[w].open()) continue;
      if (int fd = trading_[w].accept(); fd >= 0) {
        auto c = std::make_unique<Client>();
        c->conn.adopt(fd);
        c->which = w;
        Client* raw = c.get();
        c->ex_conn = ex_.connect({[raw](const std::uint8_t* p, std::size_t n) { raw->conn.write(p, n); },
                                  [raw] { raw->closing = true; }},
                                 now);
        clients_.push_back(std::move(c));
      }
    }
    std::uint8_t buf[16384];
    for (auto& c : clients_) {
      if (!c->conn.open()) continue;
      const long n = c->conn.read(buf, sizeof buf);
      if (n > 0) {
        if (c->lookup) answer_lookup(*c, buf, static_cast<std::size_t>(n));
        else ex_.on_bytes(c->ex_conn, buf, static_cast<std::size_t>(n), now);
      } else if (n < 0 && !c->lookup) {
        ex_.disconnect(c->ex_conn);
      }
      c->conn.flush();
      if (c->closing && !c->conn.has_backlog()) drop(*c);
    }
    ex_.on_timer(now);
  }

 private:
  struct Client {
    net::TcpConn conn;
    bool lookup = false;
    bool closing = false;
    int which = 0;
    int ex_conn = -1;
    std::vector<std::uint8_t> rx;
  };

  void answer_lookup(Client& c, const std::uint8_t* p, std::size_t n) {
    c.rx.insert(c.rx.end(), p, p + n);
    if (c.rx.size() < 3) return;
    const std::size_t len = load_le<std::uint16_t>(c.rx.data() + hdr::kLength);
    if (c.rx.size() < len) return;
    LookupResult r;
    r.accepted = !reject_lookups_ && valid_frame(c.rx.data(), len);
    r.reject_code = 4;
    r.primary_ip = std::string_view(ip_);
    r.secondary_ip = std::string_view(ip_);
    r.primary_port = trading_[0].port();
    r.secondary_port = trading_[1].port();
    std::uint8_t out[256];
    c.conn.write(out, encode_lookup_response(out, "OCGC", r));
    c.closing = true;
  }

  void drop(Client& c) {
    if (!c.lookup && c.ex_conn >= 0) ex_.disconnect(c.ex_conn);
    c.conn.close();
    c.closing = false;
  }

  Exchange& ex_;
  std::string ip_;
  net::TcpListener lookup_, trading_[2];
  std::vector<std::unique_ptr<Client>> clients_;
  bool reject_lookups_ = false;
};

}  // namespace obl::gw::ocgc::sim
