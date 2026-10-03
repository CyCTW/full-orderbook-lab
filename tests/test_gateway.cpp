// End to end, in memory: strategy -> OcgcGateway -> OCG-C session -> exchange simulator and back.

#include <cstdint>
#include <cstdio>
#include <deque>
#include <thread>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.hpp"
#include "obl/gw/ocgc/gateway.hpp"
#include "obl/gw/ocgc/reconcile.hpp"
#include "obl/gw/ocgc/sim/exchange.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

constexpr std::int64_t S = kScale;
constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kT0 = 1'790'000'000ull * 1'000'000'000ull;  // some UTC day, 01:33

struct Recorder {
  struct Update {
    OrderEvent event;
    OrderSlot slot;
    std::int64_t fill_qty;
  };
  std::vector<Update> updates;
  std::vector<std::pair<OrderSlot, DropReason>> dropped;
  std::vector<bool> session;
  std::vector<CloseReason> closes;
  std::vector<KillReason> kills;
  std::vector<MassCancelReport> mass;

  void on_order_update(const OrderUpdate& u, const Order&) { updates.push_back({u.event, u.slot, u.fill_qty}); }
  void on_request_dropped(OrderSlot s, ReqKind, DropReason r) { dropped.emplace_back(s, r); }
  void on_session(bool up, CloseReason why) {
    session.push_back(up);
    if (!up) closes.push_back(why);
  }
  void on_kill_switch(KillReason r) { kills.push_back(r); }
  void on_mass_cancel(const MassCancelReport& m) { mass.push_back(m); }
  int count(OrderEvent e) const {
    int n = 0;
    for (auto& u : updates) n += u.event == e;
    return n;
  }
};

struct Wire;
struct ClientTransport {
  Wire* w;
  void send(const std::uint8_t* p, std::size_t n);
  void close();
};

// Gateway <-> simulator over two in-memory queues.
struct Wire {
  std::deque<std::vector<std::uint8_t>> to_exchange, to_client;
  bool client_closed = false, server_closed = false;
};
void ClientTransport::send(const std::uint8_t* p, std::size_t n) { w->to_exchange.emplace_back(p, p + n); }
void ClientTransport::close() { w->client_closed = true; }

using Gw = OcgcGateway<ClientTransport, Recorder>;

struct Harness {
  sim::Exchange ex;
  Wire wire;
  ClientTransport tx{&wire};
  Recorder app;
  InstrumentTable ins;
  OrderTable orders;
  RiskEngine risk;
  std::unique_ptr<Gw> gw;
  int conn = -1;
  std::uint64_t now = kT0;
  SymbolIdx tencent = 0, hsbc = 0;

  static OrderTableConfig ocfg() {
    OrderTableConfig c;
    c.max_requests = 1 << 14;
    c.max_orders = 1 << 12;
    c.max_symbols = 8;
    return c;
  }

  explicit Harness(ThrottleConfig thr = {}, sim::ExchangeConfig xc = {},
                   ReplayPolicy policy = ReplayPolicy::Replay)
      : ex(xc), orders(ocfg()), risk(ins, orders, 8) {
    tencent = ins.add({"700", 100 * S, true, false, 400 * S});
    hsbc = ins.add({"5", 400 * S, false, false, 60 * S});
    ex.add_symbol("700");
    ex.add_symbol("5");
    ex.add_client("CO1", "pw");
    GatewayConfig g;
    g.session.comp_id = "CO1";
    g.session.encrypted_password = "pw";
    g.session.replay = policy;
    g.session.store_reserve_bytes = 1 << 20;
    g.session.store_reserve_msgs = 1 << 12;
    g.broker_id = "1234";
    g.bcan = "ABC123.2568";
    g.trade_date = 20261003;
    g.throttle = thr;
    gw = std::make_unique<Gw>(g, tx, app, ins, orders, risk);
  }

  void connect() {
    wire = Wire{};
    conn = ex.connect({[this](const std::uint8_t* p, std::size_t n) { wire.to_client.emplace_back(p, p + n); },
                       [this] { wire.server_closed = true; }},
                      now);
    gw->on_connected(now);
    pump();
  }
  void disconnect() {
    ex.disconnect(conn);
    gw->on_disconnected(now);
    wire = Wire{};
  }
  // Deliver everything in flight, both ways, until quiet.
  void pump() {
    for (int guard = 0; guard < 10000; ++guard) {
      if (wire.client_closed || wire.server_closed) {
        const bool server = wire.server_closed;
        wire.to_exchange.clear();
        if (wire.client_closed) ex.disconnect(conn);
        // anything the server sent before closing still arrives
        while (!wire.to_client.empty()) {
          auto m = std::move(wire.to_client.front());
          wire.to_client.pop_front();
          gw->on_bytes(m.data(), m.size(), now);
        }
        if (server) gw->on_disconnected(now);
        wire.client_closed = wire.server_closed = false;
        return;
      }
      if (!wire.to_exchange.empty()) {
        auto m = std::move(wire.to_exchange.front());
        wire.to_exchange.pop_front();
        ex.on_bytes(conn, m.data(), m.size(), now);
      } else if (!wire.to_client.empty()) {
        auto m = std::move(wire.to_client.front());
        wire.to_client.pop_front();
        gw->on_bytes(m.data(), m.size(), now);
      } else {
        return;
      }
    }
  }
  void tick(std::uint64_t dt) {
    now += dt;
    gw->poll(now);
    ex.on_timer(now);
    pump();
  }
  SendResult buy(SymbolIdx s, std::int64_t px, std::int64_t qty, OrdTif tif = OrdTif::Day) {
    auto r = gw->new_order(s, OrdSide::Buy, px, qty, now, tif);
    pump();
    return r;
  }
};

void test_order_lifecycle() {
  Harness h;
  h.connect();
  CHECK(h.gw->active());
  CHECK_EQ(h.app.session.size(), 1u);

  // rests in the book
  auto r = h.buy(h.tencent, 399 * S, 200 * S);
  CHECK(r.status == SendStatus::Sent);
  CHECK(h.orders.order(r.slot).state == OrdState::New);
  CHECK(!h.orders.exchange_id(r.slot).empty());
  auto bids = h.ex.levels("700", 1);
  CHECK(bids.size() == 1 && bids[0].price == 399 * S && bids[0].qty == 200 * S);

  // amend down keeps the order, new request ID
  const std::uint32_t id0 = h.orders.order(r.slot).req_id;
  CHECK(h.gw->amend(r.slot, 399 * S, 100 * S, h.now).status == SendStatus::Sent);
  h.pump();
  CHECK(h.orders.order(r.slot).req_id != id0);
  CHECK_EQ(h.orders.order(r.slot).leaves_qty, 100 * S);
  CHECK_EQ(h.app.count(OrderEvent::Amended), 1);

  // cancel
  CHECK(h.gw->cancel(r.slot, h.now) == SendStatus::Sent);
  h.pump();
  CHECK(h.orders.order(r.slot).state == OrdState::Cancelled);
  CHECK(h.ex.levels("700", 1).empty());
  CHECK_EQ(h.orders.position(h.tencent).open_buy_qty, 0);

  // trades against outside liquidity: 300 offered at 400.2, buy 500 -> 300 filled, 200 rests
  h.ex.add_liquidity("700", 2, 400 * S + S / 5, 300 * S);
  r = h.buy(h.tencent, 400 * S + S / 5, 500 * S);
  CHECK(h.orders.order(r.slot).state == OrdState::PartiallyFilled);
  CHECK_EQ(h.orders.position(h.tencent).pos, 300 * S);
  CHECK_EQ(h.orders.order(r.slot).leaves_qty, 200 * S);
  CHECK(h.ex.levels("700", 1)[0].qty == 200 * S);

  // amend the rest up into new liquidity: price change loses priority and matches on amend
  h.ex.add_liquidity("700", 2, 400 * S + 2 * S / 5, 200 * S);
  CHECK(h.gw->amend(r.slot, 400 * S + 2 * S / 5, 500 * S, h.now).status == SendStatus::Sent);
  h.pump();
  CHECK(h.orders.order(r.slot).state == OrdState::Filled);
  CHECK_EQ(h.orders.position(h.tencent).pos, 500 * S);

  // IOC with nothing to trade against: acked, then cancelled
  r = h.buy(h.hsbc, 59 * S, 400 * S, OrdTif::IOC);
  CHECK(h.orders.order(r.slot).state == OrdState::Cancelled);
}

void test_local_rejects() {
  Harness h;
  // session down
  CHECK(h.gw->new_order(h.tencent, OrdSide::Buy, 400 * S, 100 * S, h.now).status == SendStatus::RejectedSessionDown);
  h.connect();
  // risk: off-tick price never reaches the exchange
  auto r = h.gw->new_order(h.tencent, OrdSide::Buy, 400 * S + S / 10, 100 * S, h.now);
  CHECK(r.status == SendStatus::RejectedRisk);
  CHECK(r.risk == RiskReject::BadPrice);
  CHECK(h.wire.to_exchange.empty());
  CHECK_EQ(h.orders.order_count(), 0u);
}

void test_throttle_queue() {
  ThrottleConfig t;
  t.msgs_per_sec = 2;
  t.max_new_age_ns = 2000 * kMs;
  Harness h(t);
  h.connect();
  std::vector<SendResult> rs;
  for (int i = 0; i < 4; ++i) rs.push_back(h.buy(h.tencent, (390 + i) * S, 100 * S));
  CHECK(rs[0].status == SendStatus::Sent && rs[1].status == SendStatus::Sent);
  CHECK(rs[2].status == SendStatus::Queued && rs[3].status == SendStatus::Queued);
  CHECK(h.orders.is_unsent(rs[2].slot));
  // cancel one of the queued ones: it never goes out
  CHECK(h.gw->cancel(rs[3].slot, h.now) == SendStatus::CancelledLocally);
  CHECK(h.orders.order(rs[3].slot).state == OrdState::Rejected);
  h.tick(500 * kMs);
  CHECK(h.orders.order(rs[2].slot).state == OrdState::PendingNew);
  h.tick(600 * kMs);  // window moved
  CHECK(h.orders.order(rs[2].slot).state == OrdState::New);
  CHECK_EQ(h.ex.levels("700", 1).size(), 3u);
}

void test_throttle_expiry_and_exchange_throttle() {
  {
    ThrottleConfig t;
    t.msgs_per_sec = 1;
    t.max_new_age_ns = 100 * kMs;
    Harness h(t);
    h.connect();
    h.buy(h.tencent, 390 * S, 100 * S);
    auto q = h.buy(h.tencent, 391 * S, 100 * S);
    CHECK(q.status == SendStatus::Queued);
    h.tick(200 * kMs);  // too old before the window frees up
    CHECK_EQ(h.app.dropped.size(), 1u);
    CHECK(h.app.dropped[0].second == DropReason::Expired);
    CHECK(h.orders.order(q.slot).state == OrdState::Rejected);
  }
  {
    // gateway unthrottled, exchange limit 1/s: the second order gets a Business Message Reject
    sim::ExchangeConfig xc;
    xc.msgs_per_sec = 1;
    Harness h({}, xc);
    h.connect();
    auto a = h.buy(h.tencent, 390 * S, 100 * S);
    auto b = h.buy(h.tencent, 391 * S, 100 * S);
    CHECK(h.orders.order(a.slot).state == OrdState::New);
    CHECK(h.orders.order(b.slot).state == OrdState::Rejected);
    CHECK_EQ(h.ex.business_rejects(), 1u);
    CHECK(!h.app.dropped.empty() && h.app.dropped.back().second == DropReason::BusinessReject);
  }
}

void test_kill_switch() {
  Harness h;
  h.connect();
  auto a = h.buy(h.tencent, 390 * S, 100 * S);
  auto b = h.buy(h.hsbc, 59 * S, 400 * S);
  CHECK_EQ(h.ex.live_orders("CO1"), 2u);
  h.risk.kill_switch().engage(KillReason::Manual);
  h.tick(1 * kMs);
  CHECK_EQ(h.app.kills.size(), 1u);
  CHECK_EQ(h.app.mass.size(), 1u);
  CHECK_EQ(h.ex.live_orders("CO1"), 0u);
  CHECK(h.orders.order(a.slot).state == OrdState::Cancelled);
  CHECK(h.orders.order(b.slot).state == OrdState::Cancelled);
  auto c = h.gw->new_order(h.tencent, OrdSide::Buy, 390 * S, 100 * S, h.now);
  CHECK(c.risk == RiskReject::KillSwitch);
  h.gw->kill_switch_reset();
  CHECK(h.buy(h.tencent, 390 * S, 100 * S).status == SendStatus::Sent);
}

void test_lost_report_recovery() {
  Harness h;
  h.connect();
  h.ex.drop_next("CO1", 1);  // the ack of the next order is lost on the wire
  auto a = h.buy(h.tencent, 390 * S, 100 * S);
  CHECK(h.orders.order(a.slot).state == OrdState::PendingNew);
  // the next report reveals the gap; the session asks for a resend and catches up
  auto b = h.buy(h.tencent, 389 * S, 100 * S);
  CHECK(h.orders.order(a.slot).state == OrdState::New);
  CHECK(h.orders.order(b.slot).state == OrdState::New);
  CHECK(!h.gw->session().recovering());
  CHECK_EQ(h.gw->session().next_in_seq(), h.ex.next_out("CO1"));
}

void test_reconnect() {
  Harness h;
  h.connect();
  auto a = h.buy(h.tencent, 390 * S, 100 * S);
  // ack of the cancel is lost, then the connection drops
  h.ex.drop_next("CO1", 1);
  h.gw->cancel(a.slot, h.now);
  h.pump();
  CHECK(h.orders.order(a.slot).state == OrdState::New);  // cancel ack never arrived
  h.disconnect();
  CHECK(!h.gw->active());
  CHECK(h.gw->new_order(h.tencent, OrdSide::Buy, 390 * S, 100 * S, h.now).status == SendStatus::RejectedSessionDown);
  h.now += 10'000 * kMs;
  h.connect();  // logon recovery replays the lost cancel ack
  CHECK(h.gw->active());
  CHECK(h.orders.order(a.slot).state == OrdState::Cancelled);
  CHECK_EQ(h.gw->session().next_in_seq(), h.ex.next_out("CO1"));
  CHECK_EQ(h.gw->session().next_out_seq(), h.ex.next_in("CO1"));
}

void test_bad_checksum_and_refused_logon() {
  Harness h;
  h.connect();
  h.ex.corrupt_next("CO1");
  h.buy(h.tencent, 390 * S, 100 * S);
  CHECK(!h.gw->active());
  CHECK(!h.app.closes.empty() && h.app.closes.back() == CloseReason::BadChecksum);
  h.ex.refuse_logons(true);
  h.connect();
  CHECK(!h.gw->active());
  CHECK(h.app.closes.back() == CloseReason::LogonRejected);
}

void test_two_sessions_share_orders() {
  sim::Exchange ex;
  ex.add_symbol("700");
  ex.add_client("CO1", "pw");
  ex.add_client("CO2", "pw");
  InstrumentTable ins;
  const SymbolIdx t = ins.add({"700", 100 * S, true, false, 400 * S});
  OrderTable orders(Harness::ocfg());
  RiskEngine risk(ins, orders, 8);
  Recorder app;
  Wire w1, w2;
  ClientTransport t1{&w1}, t2{&w2};
  auto make = [&](const char* comp, ClientTransport& tx, std::uint8_t route) {
    GatewayConfig g;
    g.session.comp_id = comp;
    g.session.encrypted_password = "pw";
    g.session.store_reserve_bytes = 1 << 20;
    g.session.store_reserve_msgs = 1 << 12;
    g.broker_id = "1234";
    g.bcan = "ABC123.2568";
    g.trade_date = 20261003;
    g.route = route;
    return std::make_unique<Gw>(g, tx, app, ins, orders, risk);
  };
  auto g1 = make("CO1", t1, 0);
  auto g2 = make("CO2", t2, 1);
  std::uint64_t now = kT0;
  auto plumb = [&](Wire& w) {
    return ex.connect({[&w](const std::uint8_t* p, std::size_t n) { w.to_client.emplace_back(p, p + n); }, [] {}}, now);
  };
  const int c1 = plumb(w1), c2 = plumb(w2);
  auto pump = [&] {
    bool busy = true;
    while (busy) {
      busy = false;
      for (auto [w, g, c] : {std::tuple{&w1, g1.get(), c1}, std::tuple{&w2, g2.get(), c2}}) {
        while (!w->to_exchange.empty()) {
          auto m = w->to_exchange.front();
          w->to_exchange.pop_front();
          ex.on_bytes(c, m.data(), m.size(), now);
          busy = true;
        }
        while (!w->to_client.empty()) {
          auto m = w->to_client.front();
          w->to_client.pop_front();
          g->on_bytes(m.data(), m.size(), now);
          busy = true;
        }
      }
    }
  };
  g1->on_connected(now);
  g2->on_connected(now);
  pump();
  CHECK(g1->active() && g2->active());
  auto a = g1->new_order(t, OrdSide::Buy, 390 * S, 100 * S, now);
  auto b = g2->new_order(t, OrdSide::Buy, 389 * S, 100 * S, now);
  pump();
  CHECK_EQ(orders.route(a.slot), 0);
  CHECK_EQ(orders.route(b.slot), 1);
  CHECK(orders.order(a.slot).state == OrdState::New && orders.order(b.slot).state == OrdState::New);
  CHECK(orders.order(a.slot).req_id != orders.order(b.slot).req_id);  // one ID space
  CHECK_EQ(orders.position(t).open_buy_qty, 200 * S);                 // one exposure total
  CHECK_EQ(ex.live_orders("CO1"), 1u);
  CHECK_EQ(ex.live_orders("CO2"), 1u);
}

void test_session_down_drops_queue() {
  ThrottleConfig t;
  t.msgs_per_sec = 1;
  Harness h(t);
  h.connect();
  h.buy(h.tencent, 390 * S, 100 * S);
  auto q = h.buy(h.tencent, 391 * S, 100 * S);
  CHECK(q.status == SendStatus::Queued);
  h.disconnect();
  CHECK_EQ(h.app.dropped.size(), 1u);
  CHECK(h.app.dropped[0].second == DropReason::SessionDown);
  CHECK(h.orders.order(q.slot).state == OrdState::Rejected);
}

// A New Order lost on the way to the exchange, then a reconnect. The exchange's Logon reply
// says it expects that order's sequence number again.
void lost_outbound(ReplayPolicy policy) {
  Harness h({}, {}, policy);
  h.connect();
  auto r = h.gw->new_order(h.tencent, OrdSide::Buy, 390 * S, 100 * S, h.now);
  CHECK(r.status == SendStatus::Sent);
  h.wire.to_exchange.clear();  // lost
  h.disconnect();
  h.now += 10'000 * kMs;
  h.connect();
  CHECK(h.gw->active());
  if (policy == ReplayPolicy::Replay) {
    CHECK(h.orders.order(r.slot).state == OrdState::New);  // replayed (PossDup) and accepted
    CHECK_EQ(h.ex.live_orders("CO1"), 1u);
  } else {
    CHECK(h.orders.order(r.slot).state == OrdState::Rejected);  // gap-filled, strategy told
    CHECK(!h.app.dropped.empty() && h.app.dropped.back().second == DropReason::NotSent);
    CHECK_EQ(h.ex.live_orders("CO1"), 0u);
  }
  CHECK_EQ(h.gw->session().next_out_seq(), h.ex.next_in("CO1"));
}

void test_audit_metrics_control() {
  Harness h;
  const std::string path = "/tmp/obl_test_audit_" + std::to_string(::getpid()) + ".bin";
  AuditLog audit;
  AuditConfig ac;
  ac.path = path;
  ac.ring_bytes = 1 << 16;
  CHECK(audit.open(ac));
  h.gw->set_audit(&audit);
  ControlChannel control;
  h.gw->set_control(&control);

  h.connect();
  h.ex.add_liquidity("700", 2, 400 * S, 300 * S);
  auto a = h.buy(h.tencent, 400 * S, 200 * S);
  auto b = h.buy(h.tencent, 400 * S, 200 * S);  // 100 filled, 100 rests
  h.gw->cancel(b.slot, h.now);
  h.pump();
  CHECK(h.orders.order(a.slot).state == OrdState::Filled);
  CHECK(h.orders.order(b.slot).state == OrdState::Cancelled);

  // a control thread tightens a limit and engages the kill switch; both take effect in poll()
  std::thread ctl([&] {
    SymbolLimits L;
    L.max_order_qty = 100 * S;
    control.post([&, L] { h.risk.set_symbol_limits(h.tencent, L); });
  });
  ctl.join();
  CHECK(h.gw->new_order(h.tencent, OrdSide::Buy, 399 * S, 200 * S, h.now).status == SendStatus::Sent);  // not yet applied
  h.tick(1 * kMs);
  auto c = h.gw->new_order(h.tencent, OrdSide::Buy, 399 * S, 200 * S, h.now);
  CHECK(c.risk == RiskReject::MaxOrderQty);

  const GatewayMetrics& m = h.gw->metrics();
  CHECK_EQ(m.orders_sent.get(), 3u);
  CHECK_EQ(m.cancels_sent.get(), 1u);
  CHECK_EQ(m.fills.get(), 2u);
  CHECK_EQ(m.risk_rejects[static_cast<std::size_t>(RiskReject::MaxOrderQty)].get(), 1u);
  CHECK_EQ(m.session_ups.get(), 1u);
  CHECK(m.summary().find("fills=2") != std::string::npos);

  h.pump();
  audit.close();
  CHECK_EQ(audit.dropped(), 0u);
  std::uint64_t in = 0, out = 0;
  CHECK(read_audit(path, [&](const AuditRecord& r) { (r.dir == AuditDir::In ? in : out)++; }));
  CHECK_EQ(in, m.msgs_in.get());
  CHECK_EQ(out, m.msgs_out.get());

  // reconciliation: fills rebuilt from the audit log against a counterparty file
  auto ours = fills_from_audit(path);
  CHECK_EQ(ours.size(), 2u);
  CHECK_EQ(ours[0].qty + ours[1].qty, 300 * S);
  CHECK(reconcile(ours, ours).clean());
  auto theirs = ours;
  theirs[1].qty -= 100 * S;
  theirs.push_back({"X1", "1", "1", "700", 1, 100 * S, 400 * S, 0});
  theirs.erase(theirs.begin());
  auto rr = reconcile(ours, theirs);
  CHECK(!rr.clean());
  CHECK_EQ(rr.only_ours.size(), 1u);
  CHECK_EQ(rr.only_theirs.size(), 1u);
  CHECK_EQ(rr.mismatched.size(), 1u);
  std::remove(path.c_str());
}

}  // namespace

int main() {
  test_order_lifecycle();
  test_local_rejects();
  test_throttle_queue();
  test_throttle_expiry_and_exchange_throttle();
  test_kill_switch();
  test_lost_report_recovery();
  test_reconnect();
  test_bad_checksum_and_refused_logon();
  test_two_sessions_share_orders();
  test_session_down_drops_queue();
  lost_outbound(ReplayPolicy::Replay);
  lost_outbound(ReplayPolicy::GapFillBusiness);
  test_audit_metrics_control();
  return test_result("test_gateway");
}
