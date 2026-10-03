#pragma once

// OCG-C exchange simulator: the server side of the binary session plus a price-time matching
// engine. For tests and end-to-end runs of the gateway; not a model of OTP-C internals.
//
// Session (per Comp ID, sequence numbers kept across reconnects like the real OCG-C):
//   Logon checks (password, Next Expected), §5.3 recovery both ways, heartbeat / test request,
//   gap detection with Resend Request, replay of Execution Reports with PossDup, gap fill of
//   session messages, Logout, throttle (Business Message Reject when over msgs_per_sec).
// Matching: limit orders, price-time priority, Day / IOC / FOK, amend (price change or quantity
//   increase loses priority), cancel, mass cancel; resting liquidity from other participants can
//   be added with add_liquidity().
// Faults: drop or corrupt the next outbound messages, refuse logons.
//
// Wire it with any transport: Connection::send(bytes) towards the client and
// Exchange::on_bytes(conn, bytes) for what the client sent.

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "obl/gw/instrument.hpp"
#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/session.hpp"

namespace obl::gw::ocgc::sim {

struct ExchangeConfig {
  std::string comp_id = "OCGC";
  std::uint32_t trade_date = 20261003;
  std::uint32_t msgs_per_sec = 0;  // per client; 0 = unlimited
  bool check_tick = true;
  // Logon password check: (field from the Logon, expected password, now) -> ok. Default: the
  // field must equal the password (tests without encryption). With RSA, decrypt and check the
  // login-time prefix (see password.hpp).
  std::function<bool(std::string_view, const std::string&, std::uint64_t)> verify_password;
};

// What the simulator writes to a client connection.
struct Connection {
  std::function<void(const std::uint8_t*, std::size_t)> send;
  std::function<void()> close;
};

class Exchange {
 public:
  explicit Exchange(ExchangeConfig cfg = {}) : cfg_(std::move(cfg)) {}

  void add_client(const std::string& comp_id, const std::string& password) {
    clients_[comp_id].password = password;
  }
  void add_symbol(const std::string& security_id) { books_[security_id]; }

  // Resting liquidity from someone else (no reports are sent for it).
  void add_liquidity(const std::string& sec, std::uint8_t side, std::int64_t price, std::int64_t qty) {
    Resting r;
    r.order_id = ++next_order_id_;
    r.side = side;
    r.price = price;
    r.qty = r.leaves = qty;
    insert(books_[sec], r);
  }

  // --- connection events ------------------------------------------------------------------------

  int connect(Connection c, std::uint64_t now) {
    conns_.push_back({std::move(c), nullptr, now, {}});
    return static_cast<int>(conns_.size() - 1);
  }
  void disconnect(int conn) {
    Conn& c = conns_[conn];
    if (c.client && c.client->conn == conn) c.client->conn = -1;
    c.client = nullptr;
  }

  void on_bytes(int conn, const std::uint8_t* p, std::size_t n, std::uint64_t now) {
    now_ = now;
    Conn& c = conns_[conn];
    c.rx.insert(c.rx.end(), p, p + n);
    std::size_t off = 0;
    while (c.rx.size() - off >= 3) {
      const std::uint8_t* m = c.rx.data() + off;
      const std::size_t len = load_le<std::uint16_t>(m + hdr::kLength);
      if (m[0] != kStx || len < hdr::kSize + kTrailerSize) return kill_conn(conn);
      if (c.rx.size() - off < len) break;
      if (!verify_checksum(m, len)) return kill_conn(conn);  // §4.8: drop without Logout
      if (!process(conn, m, len)) return;
      off += len;
    }
    c.rx.erase(c.rx.begin(), c.rx.begin() + static_cast<std::ptrdiff_t>(off));
  }

  void on_timer(std::uint64_t now) {
    now_ = now;
    for (auto& [id, cl] : clients_)
      if (cl.conn >= 0 && cl.active && now - cl.last_send >= heartbeat_ns_) send_admin(cl, encode_heartbeat(buf_, cl.next_out, cfg_.comp_id));
  }

  // --- fault injection -----------------------------------------------------------------------------

  // The next n messages to this client get a sequence number but are not delivered (a gap).
  void drop_next(const std::string& comp_id, int n) { clients_[comp_id].drop_next = n; }
  // The next message to this client is sent with a broken checksum.
  void corrupt_next(const std::string& comp_id) { clients_[comp_id].corrupt_next = true; }
  void refuse_logons(bool on) { refuse_logons_ = on; }
  // Exchange-initiated logout of a client.
  void logout_client(const std::string& comp_id) {
    Client& cl = clients_[comp_id];
    if (cl.conn < 0) return;
    send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id, "exchange logout"));
  }

  // --- inspection ----------------------------------------------------------------------------------

  struct BookLevel {
    std::int64_t price, qty;
    std::size_t orders;
  };
  std::vector<BookLevel> levels(const std::string& sec, std::uint8_t side) const {
    std::vector<BookLevel> out;
    auto it = books_.find(sec);
    if (it == books_.end()) return out;
    auto emit = [&](const auto& m) {
      for (const auto& [px, q] : m) {
        std::int64_t t = 0;
        for (const Resting& r : q) t += r.leaves;
        out.push_back({px, t, q.size()});
      }
    };
    if (side == 1) emit(it->second.bids);
    else emit(it->second.asks);
    return out;
  }
  std::uint32_t next_in(const std::string& comp_id) { return clients_[comp_id].next_in; }
  std::uint32_t next_out(const std::string& comp_id) { return clients_[comp_id].next_out; }
  std::uint64_t business_rejects() const { return business_rejects_; }
  std::size_t live_orders(const std::string& comp_id) const {
    auto it = clients_.find(comp_id);
    return it == clients_.end() ? 0 : it->second.orders.size();
  }

 private:
  // --- book ------------------------------------------------------------------------------------------
  struct Resting {
    std::uint64_t order_id = 0;
    std::string owner;  // Comp ID, empty = outside liquidity
    std::string sec;
    std::uint32_t cl_ord_id = 0;
    std::uint8_t side = 1;
    std::uint8_t tif = 0;
    std::int64_t price = 0, qty = 0, leaves = 0, cum = 0;
  };
  using Level = std::list<Resting>;
  struct Book {
    std::map<std::int64_t, Level, std::greater<>> bids;
    std::map<std::int64_t, Level> asks;
  };
  struct Loc {
    std::string sec;
    std::uint8_t side;
    std::int64_t price;
    Level::iterator it;
  };

  struct Client {
    std::string password;
    std::uint32_t next_out = 1, next_in = 1;
    MessageStore store;  // everything sent to this client today
    int conn = -1;
    bool active = false;
    bool resend_outstanding = false;
    std::uint64_t last_send = 0;
    int drop_next = 0;
    bool corrupt_next = false;
    SlidingWindow throttle;
    std::unordered_map<std::uint32_t, std::uint64_t> orders;  // current ClOrdID -> order id
    std::unordered_map<std::uint32_t, bool> seen_ids;         // every ClOrdID used today
  };
  struct Conn {
    Connection io;
    Client* client;
    std::uint64_t since;
    std::vector<std::uint8_t> rx;
  };

  static void insert(Book& b, const Resting& r) {
    if (r.side == 1) b.bids[r.price].push_back(r);
    else b.asks[r.price].push_back(r);
  }

  // --- session ---------------------------------------------------------------------------------

  bool process(int conn, const std::uint8_t* m, std::size_t len) {
    Conn& c = conns_[conn];
    const Header h = read_header(m);
    if (!c.client) {  // first message must be a Logon
      if (h.type != MsgType::Logon) {
        kill_conn(conn);
        return false;
      }
      return on_logon(conn, h, m, len);
    }
    Client& cl = *c.client;
    SessionFields sf;
    if (is_admin(h.type)) decode_session(m, len, h.type, sf);

    if (h.type == MsgType::SequenceReset && sf.gap_fill != sequence_reset::kGapFill) {
      send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id, "client reset not allowed"));
      return close_client(cl), false;
    }
    if (h.seq > cl.next_in) {
      if (!cl.resend_outstanding) {
        send_admin(cl, encode_resend_request(buf_, cl.next_out, cfg_.comp_id, cl.next_in, 0));
        cl.resend_outstanding = true;
      }
      return true;
    }
    if (h.seq < cl.next_in) {
      if (h.poss_dup) return true;
      send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id, "sequence number too low"));
      return close_client(cl), false;
    }
    if (h.type == MsgType::SequenceReset) {
      cl.next_in = sf.new_seq;
      cl.resend_outstanding = false;
      return true;
    }
    ++cl.next_in;
    cl.resend_outstanding = false;
    switch (h.type) {
      case MsgType::Heartbeat: return true;
      case MsgType::TestRequest:
        send_admin(cl, encode_heartbeat(buf_, cl.next_out, cfg_.comp_id, sf.test_req_id));
        return true;
      case MsgType::ResendRequest:
        replay(cl, sf.start_seq, sf.end_seq ? sf.end_seq : cl.next_out - 1);
        return true;
      case MsgType::Logout:
        send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id));
        return close_client(cl), false;
      default:
        break;
    }
    // business message
    if (!cl.throttle.unlimited() && !cl.throttle.try_take(now_)) {
      OrderRequest rq;
      decode_request(m, len, h.type, rq);
      business_reject(cl, h, rq.cl_ord_id, "throttle exceeded");
      return true;
    }
    OrderRequest rq;
    if (!decode_request(m, len, h.type, rq)) return true;
    const std::string owner = std::string(read_alnum(m + hdr::kCompId, hdr::kCompIdSize));
    switch (h.type) {
      case MsgType::NewOrder: new_order(cl, owner, rq); break;
      case MsgType::CancelOrder: cancel(cl, rq); break;
      case MsgType::AmendOrder: amend(cl, owner, rq); break;
      case MsgType::MassCancel: mass_cancel(cl, rq); break;
      default: business_reject(cl, h, rq.cl_ord_id, "unsupported message"); break;
    }
    return true;
  }

  bool on_logon(int conn, const Header& h, const std::uint8_t* m, std::size_t len) {
    Conn& c = conns_[conn];
    auto it = clients_.find(std::string(h.comp_id));
    if (it == clients_.end()) {  // §4.1: unknown Comp ID -> terminate without a message
      kill_conn(conn);
      return false;
    }
    Client& cl = it->second;
    SessionFields sf;
    std::string_view pw;
    for_each_field_unchecked(m, len, logon::kFields, [&](const FieldRef& f) {
      if (f.bit == logon::Password) pw = as_str(f);
    });
    decode_session(m, len, MsgType::Logon, sf);
    c.client = &cl;
    cl.conn = conn;
    cl.throttle.configure(cfg_.msgs_per_sec, 1'000'000'000);
    const bool pw_ok = cfg_.verify_password ? cfg_.verify_password(pw, cl.password, now_) : pw == cl.password;
    if (refuse_logons_ || !pw_ok) {
      send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id, "invalid password"));
      close_client(cl);
      return false;
    }
    if (sf.next_expected > cl.next_out || h.seq < cl.next_in) {
      send_admin(cl, encode_logout(buf_, cl.next_out, cfg_.comp_id, "sequence mismatch"));
      close_client(cl);
      return false;
    }
    if (h.seq == cl.next_in) ++cl.next_in;
    else cl.resend_outstanding = true;  // client replays [next_in, h.seq) and gap-fills its Logon
    cl.active = true;
    const std::uint32_t logon_seq = cl.next_out;
    send_admin(cl, encode_logon_reply(buf_, logon_seq, cfg_.comp_id, cl.next_in));
    if (sf.next_expected < logon_seq) {
      // §5.3 case 3: resend what the client missed, then skip the Logon reply's own number
      replay(cl, sf.next_expected, logon_seq - 1);
      send_raw(cl, buf_, encode_sequence_reset(buf_, logon_seq, cfg_.comp_id, logon_seq + 1, true, true));
    }
    return true;
  }

  void replay(Client& cl, std::uint32_t from, std::uint32_t to) {
    std::uint8_t tmp[4096];
    std::uint32_t seq = from;
    while (seq <= to && seq < cl.next_out) {
      if (is_admin(cl.store.type(seq))) {
        std::uint32_t end = seq;
        while (end + 1 <= to && end + 1 < cl.next_out && is_admin(cl.store.type(end + 1))) ++end;
        send_raw(cl, tmp, encode_sequence_reset(tmp, seq, cfg_.comp_id, end + 1, true, true));
        seq = end + 1;
      } else {
        const std::size_t n = cl.store.size(seq);
        std::memcpy(tmp, cl.store.data(seq), n);
        tmp[hdr::kPossDup] = 1;
        store_le<std::uint32_t>(tmp + n - kTrailerSize, checksum(tmp, n - kTrailerSize));
        send_raw(cl, tmp, n);
        ++seq;
      }
    }
  }

  // New sequenced message to the client (stored for replay; subject to fault injection).
  void send_admin(Client& cl, std::size_t n) { send_seq(cl, buf_, n); }
  void send_seq(Client& cl, std::uint8_t* m, std::size_t n) {
    store_le<std::uint32_t>(m + hdr::kSeqNum, cl.next_out);
    store_le<std::uint32_t>(m + n - kTrailerSize, checksum(m, n - kTrailerSize));
    cl.store.append(cl.next_out++, m, n);
    cl.last_send = now_;
    if (cl.drop_next > 0) {
      --cl.drop_next;
      return;
    }
    send_raw(cl, m, n);
  }
  void send_raw(Client& cl, const std::uint8_t* m, std::size_t n) {
    if (cl.conn < 0) return;
    Conn& c = conns_[cl.conn];
    if (cl.corrupt_next) {
      cl.corrupt_next = false;
      std::vector<std::uint8_t> bad(m, m + n);
      bad[n - 1] ^= 0xFF;
      c.io.send(bad.data(), n);
      return;
    }
    c.io.send(m, n);
  }

  void close_client(Client& cl) {
    cl.active = false;
    if (cl.conn >= 0) {
      Conn& c = conns_[cl.conn];
      c.client = nullptr;
      if (c.io.close) c.io.close();
    }
    cl.conn = -1;
  }
  void kill_conn(int conn) {
    Conn& c = conns_[conn];
    if (c.client) {
      c.client->active = false;
      c.client->conn = -1;
    }
    c.client = nullptr;
    if (c.io.close) c.io.close();
  }

  void business_reject(Client& cl, const Header& h, std::uint32_t id, std::string_view reason) {
    ++business_rejects_;
    Writer w(buf_, MsgType::BusinessMessageReject, 0, cfg_.comp_id);
    w.u16(business_reject::BusinessRejectCode, 0)
        .var_alnum(business_reject::Reason, reason)
        .u8(business_reject::RefMsgType, static_cast<std::uint8_t>(h.type))
        .u32(business_reject::RefSeqNum, h.seq);
    if (id) {
      char* p = w.alnum_slot(business_reject::BusinessRejectRefId, field_size::kClOrdId);
      std::to_chars(p, p + 20, id);
    }
    send_seq(cl, buf_, w.finish());
  }

  // --- execution reports ---------------------------------------------------------------------------

  std::string time_str() const {
    char t[32] = {};
    format_transact_time(t, cfg_.trade_date, us_of_day_utc());
    return std::string(t, 24);
  }
  std::uint64_t us_of_day_utc() const { return now_ / 1000 % 86'400'000'000ull; }

  void report(Client& cl, ExecReport& er) {
    namespace b = exec_report;
    er.present |= 1ull << b::SubmittingBrokerId | 1ull << b::SecurityIdSource | 1ull << b::SecurityExchange |
                  1ull << b::TransactTime | 1ull << b::ExecId;
    const std::string t = time_str();
    const std::string x = std::to_string(++next_exec_id_);
    er.transact_time = std::string_view(t);
    er.exec_id = std::string_view(x);
    if (er.submitting_broker_id.p == nullptr) er.submitting_broker_id = "1234";
    send_seq(cl, buf_, encode_exec_report(buf_, 0, cfg_.comp_id, er));
  }

  // Common fields of a report about resting order r.
  static void base(ExecReport& er, const Resting& r, const std::string& cl_id, const std::string& ord_id) {
    namespace b = exec_report;
    er.present = 1ull << b::ClOrdId | 1ull << b::SecurityId | 1ull << b::Side | 1ull << b::OrderId |
                 1ull << b::OrdStatus | 1ull << b::ExecType | 1ull << b::CumQty | 1ull << b::LeavesQty;
    er.cl_ord_id = std::string_view(cl_id);
    er.security_id = std::string_view(r.sec);
    er.side = r.side;
    er.order_id = std::string_view(ord_id);
    er.cum_qty = r.cum;
    er.leaves_qty = r.leaves;
  }

  void send_simple(Client& cl, const Resting& r, std::uint32_t cl_ord_id, std::uint32_t orig, ExecType et,
                   OrdStatus os, std::uint16_t code = 0) {
    namespace b = exec_report;
    const std::string cl_id = std::to_string(cl_ord_id), orig_id = std::to_string(orig),
                      ord_id = std::to_string(r.order_id);
    ExecReport er;
    base(er, r, cl_id, ord_id);
    er.exec_type = et;
    er.ord_status = os;
    if (orig) {
      er.present |= 1ull << b::OrigClOrdId;
      er.orig_cl_ord_id = std::string_view(orig_id);
    }
    if (et == ExecType::New || et == ExecType::Amend) {
      er.present |= 1ull << b::OrdType | 1ull << b::Price | 1ull << b::OrderQty;
      er.ord_type = 2;
      er.price = r.price;
      er.order_qty = r.qty;
    }
    if (et == ExecType::Reject) {
      er.present |= 1ull << b::OrderRejectCode;
      er.order_reject_code = code;
    } else if (et == ExecType::CancelReject) {
      er.present |= 1ull << b::CancelRejectCode;
      er.cancel_reject_code = code;
    } else if (et == ExecType::AmendReject) {
      er.present |= 1ull << b::AmendRejectCode;
      er.amend_reject_code = code;
    }
    report(cl, er);
  }

  void send_trade(Client& cl, const Resting& r, std::int64_t qty, std::int64_t px, bool aggressor) {
    namespace b = exec_report;
    const std::string cl_id = std::to_string(r.cl_ord_id), ord_id = std::to_string(r.order_id),
                      match = std::to_string(next_exec_id_ + 1);
    ExecReport er;
    base(er, r, cl_id, ord_id);
    er.present |= 1ull << b::OrdType | 1ull << b::Price | 1ull << b::OrderQty | 1ull << b::MatchType |
                  1ull << b::ExecQty | 1ull << b::ExecPrice | 1ull << b::TradeMatchId | 1ull << b::AggressorIndicator;
    er.exec_type = ExecType::Trade;
    er.ord_status = r.leaves ? OrdStatus::PartiallyFilled : OrdStatus::Filled;
    er.ord_type = 2;
    er.price = r.price;
    er.order_qty = r.qty;
    er.match_type = 4;
    er.exec_qty = qty;
    er.exec_price = px;
    er.trade_match_id = std::string_view(match);
    er.aggressor = aggressor;
    report(cl, er);
  }

  // --- order handling --------------------------------------------------------------------------------

  void new_order(Client& cl, const std::string& owner, const OrderRequest& rq) {
    Resting r;
    r.order_id = ++next_order_id_;
    r.owner = owner;
    r.sec = std::string(rq.security_id.view());
    r.cl_ord_id = rq.cl_ord_id;
    r.side = rq.side == 5 ? 2 : rq.side;
    r.tif = rq.tif;
    r.price = rq.price;
    r.qty = r.leaves = rq.qty;
    auto bk = books_.find(r.sec);
    std::uint16_t code = 0;
    if (cl.seen_ids.count(rq.cl_ord_id)) code = 6;  // duplicate order
    else if (bk == books_.end()) code = 99;
    else if (rq.qty <= 0 || (cfg_.check_tick && !hkex_valid_price(rq.price))) code = 99;
    cl.seen_ids[rq.cl_ord_id] = true;
    if (code) {
      r.leaves = 0;
      send_simple(cl, r, rq.cl_ord_id, 0, ExecType::Reject, OrdStatus::Rejected, code);
      return;
    }
    Book& b = bk->second;
    if (r.tif == 4 && available(b, r) < r.qty) {  // FOK that cannot fill completely
      send_simple(cl, r, rq.cl_ord_id, 0, ExecType::New, OrdStatus::New);
      r.leaves = 0;
      send_simple(cl, r, rq.cl_ord_id, 0, ExecType::Cancel, OrdStatus::Cancelled);
      return;
    }
    send_simple(cl, r, rq.cl_ord_id, 0, ExecType::New, OrdStatus::New);
    match(b, r);
    if (r.leaves == 0) return;
    if (r.tif == 3 || r.tif == 4) {  // IOC remainder
      r.leaves = 0;
      send_simple(cl, r, r.cl_ord_id, 0, ExecType::Cancel, OrdStatus::Cancelled);
      return;
    }
    rest(b, r, cl);
  }

  void rest(Book& b, const Resting& r, Client& cl) {
    insert(b, r);
    Level& lvl = r.side == 1 ? b.bids[r.price] : b.asks[r.price];
    locs_[r.order_id] = {r.sec, r.side, r.price, std::prev(lvl.end())};
    cl.orders[r.cl_ord_id] = r.order_id;
  }

  static std::int64_t available(Book& b, const Resting& r) {
    std::int64_t q = 0;
    auto add = [&](auto& side, auto crosses) {
      for (auto& [px, lvl] : side) {
        if (!crosses(px)) break;
        for (auto& o : lvl) q += o.leaves;
      }
    };
    if (r.side == 1) add(b.asks, [&](std::int64_t px) { return px <= r.price; });
    else add(b.bids, [&](std::int64_t px) { return px >= r.price; });
    return q;
  }

  void match(Book& b, Resting& r) {
    auto run = [&](auto& side, auto crosses) {
      while (r.leaves > 0 && !side.empty() && crosses(side.begin()->first)) {
        Level& lvl = side.begin()->second;
        Resting& o = lvl.front();
        const std::int64_t q = std::min(r.leaves, o.leaves);
        const std::int64_t px = o.price;
        r.leaves -= q;
        r.cum += q;
        o.leaves -= q;
        o.cum += q;
        if (auto it = clients_.find(r.owner); it != clients_.end()) send_trade(it->second, r, q, px, true);
        if (auto it = clients_.find(o.owner); it != clients_.end()) send_trade(it->second, o, q, px, false);
        if (o.leaves == 0) {
          if (auto it = clients_.find(o.owner); it != clients_.end()) it->second.orders.erase(o.cl_ord_id);
          locs_.erase(o.order_id);
          lvl.pop_front();
          if (lvl.empty()) side.erase(side.begin());
        }
      }
    };
    if (r.side == 1) run(b.asks, [&](std::int64_t px) { return px <= r.price; });
    else run(b.bids, [&](std::int64_t px) { return px >= r.price; });
  }

  Resting* find(Client& cl, std::uint32_t cl_ord_id) {
    auto it = cl.orders.find(cl_ord_id);
    if (it == cl.orders.end()) return nullptr;
    auto l = locs_.find(it->second);
    return l == locs_.end() ? nullptr : &*l->second.it;
  }

  void remove(Client& cl, Resting& r) {
    const Loc loc = locs_[r.order_id];
    Book& b = books_[loc.sec];
    cl.orders.erase(r.cl_ord_id);
    locs_.erase(r.order_id);
    if (loc.side == 1) {
      auto lv = b.bids.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) b.bids.erase(lv);
    } else {
      auto lv = b.asks.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) b.asks.erase(lv);
    }
  }

  void cancel(Client& cl, const OrderRequest& rq) {
    cl.seen_ids[rq.cl_ord_id] = true;
    Resting* r = find(cl, rq.orig_cl_ord_id);
    if (!r) {
      Resting dummy;
      dummy.sec = std::string(rq.security_id.view());
      dummy.side = rq.side;
      send_simple(cl, dummy, rq.cl_ord_id, rq.orig_cl_ord_id, ExecType::CancelReject, OrdStatus::Rejected, 1);
      return;
    }
    Resting copy = *r;
    remove(cl, *r);
    copy.leaves = 0;
    send_simple(cl, copy, rq.cl_ord_id, rq.orig_cl_ord_id, ExecType::Cancel, OrdStatus::Cancelled);
  }

  void amend(Client& cl, const std::string&, const OrderRequest& rq) {
    cl.seen_ids[rq.cl_ord_id] = true;
    Resting* r = find(cl, rq.orig_cl_ord_id);
    if (!r || rq.qty <= r->cum || (cfg_.check_tick && !hkex_valid_price(rq.price))) {
      Resting dummy = r ? *r : Resting{};
      if (!r) {
        dummy.sec = std::string(rq.security_id.view());
        dummy.side = rq.side;
      }
      send_simple(cl, dummy, rq.cl_ord_id, rq.orig_cl_ord_id, ExecType::AmendReject, OrdStatus::Rejected, r ? 99 : 1);
      return;
    }
    Resting n = *r;
    const bool keeps_priority = rq.price == r->price && rq.qty <= r->qty;
    n.price = rq.price;
    n.qty = rq.qty;
    n.leaves = rq.qty - r->cum;
    n.cl_ord_id = rq.cl_ord_id;
    if (keeps_priority) {
      cl.orders.erase(r->cl_ord_id);
      *r = n;
      cl.orders[n.cl_ord_id] = n.order_id;
      send_simple(cl, n, rq.cl_ord_id, rq.orig_cl_ord_id, ExecType::Amend, OrdStatus::New);
      return;
    }
    remove(cl, *r);
    send_simple(cl, n, rq.cl_ord_id, rq.orig_cl_ord_id, ExecType::Amend,
                n.cum ? OrdStatus::PartiallyFilled : OrdStatus::New);
    Book& b = books_[n.sec];
    match(b, n);
    if (n.leaves > 0) rest(b, n, cl);
  }

  void mass_cancel(Client& cl, const OrderRequest& rq) {
    cl.seen_ids[rq.cl_ord_id] = true;
    std::vector<std::uint32_t> ids;
    for (auto& [cid, oid] : cl.orders) {
      auto l = locs_.find(oid);
      if (l == locs_.end()) continue;
      if (rq.mass_type == mass_cancel::kForSecurity && l->second.sec != rq.security_id.view()) continue;
      if (rq.side && l->second.side != rq.side) continue;
      ids.push_back(cid);
    }
    for (std::uint32_t cid : ids) {
      Resting* r = find(cl, cid);
      Resting copy = *r;
      remove(cl, *r);
      copy.leaves = 0;
      send_simple(cl, copy, cid, 0, ExecType::Cancel, OrdStatus::Cancelled);
    }
    MassCancelReport m;
    m.cl_ord_id = rq.cl_ord_id;
    m.request_type = rq.mass_type;
    m.response = rq.mass_type;
    const std::string rid = std::to_string(++next_exec_id_);
    m.report_id = std::string_view(rid);
    send_seq(cl, buf_, encode_mass_cancel_report(buf_, 0, cfg_.comp_id, m, time_str()));
  }

  ExchangeConfig cfg_;
  std::unordered_map<std::string, Client> clients_;
  std::vector<Conn> conns_;
  std::map<std::string, Book> books_;
  std::unordered_map<std::uint64_t, Loc> locs_;
  std::uint64_t next_order_id_ = 9'000'000;
  std::uint64_t next_exec_id_ = 0;
  std::uint64_t business_rejects_ = 0;
  std::uint64_t now_ = 0;
  std::uint64_t heartbeat_ns_ = 20'000'000'000;
  bool refuse_logons_ = false;
  alignas(64) std::uint8_t buf_[1024];
};

}  // namespace obl::gw::ocgc::sim
