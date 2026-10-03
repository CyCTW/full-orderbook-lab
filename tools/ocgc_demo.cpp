// The whole gateway over real TCP in one process: exchange simulator (Lookup + primary + mirror)
// and gateway (connector, session, risk, throttle, order table, audit log).
//
//   obl_ocgc_demo [audit-file]
//
// Script: logon via Lookup; a resting order; a trade against outside liquidity; an amend; a
// burst that hits the local throttle; a risk reject; failover to the mirror; kill switch; logout.

#include <cstdio>
#include <functional>
#include <string>

#include "obl/gw/audit.hpp"
#include "obl/gw/net/tcp.hpp"
#include "obl/gw/ocgc/connector.hpp"
#include "obl/gw/ocgc/gateway.hpp"
#include "obl/gw/ocgc/sim/server.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

constexpr std::int64_t S = kScale;
constexpr std::uint64_t kMs = 1'000'000;

const char* event_name(OrderEvent e) {
  switch (e) {
    case OrderEvent::Acked: return "acked";
    case OrderEvent::Rejected: return "rejected";
    case OrderEvent::Filled: return "filled";
    case OrderEvent::Cancelled: return "cancelled";
    case OrderEvent::Expired: return "expired";
    case OrderEvent::Amended: return "amended";
    case OrderEvent::CancelRejected: return "cancel rejected";
    case OrderEvent::AmendRejected: return "amend rejected";
    case OrderEvent::FillBusted: return "fill busted";
    case OrderEvent::Duplicate: return "duplicate";
    case OrderEvent::Unknown: return "unknown";
  }
  return "?";
}

struct Printer {
  void on_order_update(const OrderUpdate& u, const Order& o) {
    std::printf("  order %u %-10s", u.slot, event_name(u.event));
    if (u.event == OrderEvent::Filled) std::printf(" %.0f @ %.2f", u.fill_qty / 1e8, u.fill_price / 1e8);
    std::printf("  (leaves %.0f, cum %.0f)\n", o.leaves_qty / 1e8, o.cum_qty / 1e8);
  }
  void on_request_dropped(OrderSlot s, ReqKind, DropReason r) { std::printf("  order %u request dropped (%d)\n", s, static_cast<int>(r)); }
  void on_session(bool up, CloseReason why) {
    if (up) std::printf("  session UP\n");
    else std::printf("  session DOWN (%s)\n", to_string(why));
  }
  void on_kill_switch(KillReason r) { std::printf("  KILL SWITCH: %s\n", to_string(r)); }
  void on_mass_cancel(const MassCancelReport& m) { std::printf("  mass cancel report: response %u\n", m.response); }
};

using Gw = OcgcGateway<ConnTransport, Printer>;

}  // namespace

int main(int argc, char** argv) {
  const std::string audit_path = argc > 1 ? argv[1] : "ocgc_demo_audit.bin";

  sim::Exchange ex;
  ex.add_symbol("700");
  ex.add_client("CO99999901", "secret");
  sim::Server srv(ex);
  if (!srv.start()) {
    std::fprintf(stderr, "cannot listen on 127.0.0.1\n");
    return 1;
  }

  InstrumentTable ins;
  const SymbolIdx tencent = ins.add({"700", 100 * S, true, false, 400 * S});
  OrderTableConfig oc;
  oc.max_requests = 1 << 16;
  oc.max_orders = 1 << 14;
  oc.max_symbols = 16;
  OrderTable orders(oc);
  RiskEngine risk(ins, orders, 16);
  SymbolLimits lim;
  lim.max_order_qty = 10'000 * S;
  lim.collar_bps = 300;
  risk.set_symbol_limits(tencent, lim);

  Printer app;
  ConnTransport tx;
  GatewayConfig g;
  g.session.comp_id = "CO99999901";
  g.session.encrypted_password = "secret";
  g.session.store_reserve_bytes = 8 << 20;
  g.session.store_reserve_msgs = 1 << 16;
  g.broker_id = "1234";
  g.bcan = "ABC123.2568";
  g.trade_date = 20261003;
  g.throttle.msgs_per_sec = 5;
  g.throttle.max_new_age_ns = 2000 * kMs;  // let queued orders wait for the window in this demo
  Gw gw(g, tx, app, ins, orders, risk);
  gw.prepare_templates(tencent);

  AuditLog audit;
  AuditConfig ac;
  ac.path = audit_path;
  audit.open(ac);
  gw.set_audit(&audit);

  ConnectorConfig cc;
  cc.comp_id = "CO99999901";
  cc.lookup = {{"127.0.0.1", srv.lookup_port()}};
  cc.reconnect_delay_ns = 100 * kMs;
  Connector<Gw> conn(cc, gw, tx);

  auto run = [&](std::uint64_t ms, const std::function<bool()>& until = {}) {
    const std::uint64_t end = net::wall_ns() + ms * kMs;
    while (net::wall_ns() < end) {
      const std::uint64_t now = net::wall_ns();
      srv.step(now);
      conn.step(now);
      if (until && until()) return;
    }
  };
  auto now = [] { return net::wall_ns(); };

  std::printf("connect via Lookup Service on port %u\n", srv.lookup_port());
  conn.start(now());
  run(2000, [&] { return gw.active(); });

  std::printf("\nbuy 200 @ 399.0 (rests)\n");
  auto a = gw.new_order(tencent, OrdSide::Buy, 399 * S, 200 * S, now());
  run(50);

  std::printf("\noutside seller offers 300 @ 400.0; buy 500 @ 400.0\n");
  ex.add_liquidity("700", 2, 400 * S, 300 * S);
  auto b = gw.new_order(tencent, OrdSide::Buy, 400 * S, 500 * S, now());
  run(50);

  std::printf("\namend the resting 200 @ 399.0 down to 100\n");
  gw.amend(a.slot, 399 * S, 100 * S, now());
  run(50);

  std::printf("\nburst of 6 orders, local throttle 5/s\n");
  for (int i = 0; i < 6; ++i) {
    auto r = gw.new_order(tencent, OrdSide::Buy, (395 - i) * S, 100 * S, now());
    std::printf("  order %u -> %s\n", r.slot, r.status == SendStatus::Sent ? "sent" : r.status == SendStatus::Queued ? "queued" : "rejected");
  }
  run(1200);

  std::printf("\nrisk: buy at 420 (5%% above reference, collar 3%%)\n");
  auto c = gw.new_order(tencent, OrdSide::Buy, 420 * S, 100 * S, now());
  std::printf("  -> %s\n", to_string(c.risk));

  std::printf("\nprimary trading service fails\n");
  srv.fail_primary();
  run(3000, [&] { return conn.target() == 1 && gw.active(); });
  std::printf("  reconnected to the mirror; position %.0f, live orders %u\n", orders.position(tencent).pos / 1e8,
              orders.position(tencent).live_orders);

  std::printf("\nkill switch\n");
  risk.kill_switch().engage(KillReason::Manual);
  run(200);
  std::printf("  live orders now %u\n", orders.position(tencent).live_orders);

  std::printf("\nend of day\n");
  conn.stop();
  gw.logout(now());
  run(500, [&] { return conn.state() == Connector<Gw>::State::Idle; });
  audit.close();

  std::printf("\n%s\n", gw.metrics().summary().c_str());
  std::printf("audit log: %s (%llu messages)\n", audit_path.c_str(), static_cast<unsigned long long>(audit.written()));
  (void)b;
  return 0;
}
