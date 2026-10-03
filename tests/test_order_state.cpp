// Order table: request IDs, state machine, exposure, positions; HKEX tick table.

#include <cstdint>
#include <random>
#include <vector>

#include "check.hpp"
#include "obl/gw/ocgc/report_adapter.hpp"
#include "obl/gw/order_state.hpp"

using namespace obl::gw;

namespace {

constexpr std::int64_t S = kScale;

OrderTableConfig small() {
  OrderTableConfig c;
  c.max_requests = 1 << 16;
  c.max_orders = 1 << 14;
  c.max_symbols = 16;
  return c;
}

Report rep(ReportKind k, std::uint32_t id) {
  Report r;
  r.kind = k;
  r.req_id = id;
  return r;
}
Report fill(std::uint32_t id, std::int64_t qty, std::int64_t px, std::int64_t cum, std::int64_t leaves) {
  Report r = rep(ReportKind::Fill, id);
  r.exec_qty = qty;
  r.exec_price = px;
  r.cum_qty = cum;
  r.leaves_qty = leaves;
  return r;
}

void test_tick_table() {
  CHECK_EQ(hkex_tick(S / 10), S / 1000);   // 0.10 -> 0.001
  CHECK_EQ(hkex_tick(S / 4), S / 1000);    // 0.25 edge belongs to the lower band
  CHECK_EQ(hkex_tick(S / 2), S / 200);     // 0.50 -> 0.005
  CHECK_EQ(hkex_tick(5 * S), S / 100);
  CHECK_EQ(hkex_tick(15 * S), S / 50);
  CHECK_EQ(hkex_tick(400 * S), S / 5);     // e.g. 700.HK at 400 -> 0.2
  CHECK_EQ(hkex_tick(9995 * S), 5 * S);
  CHECK_EQ(hkex_tick(10000 * S), 0);
  CHECK(hkex_valid_price(400 * S + S / 5));
  CHECK(!hkex_valid_price(400 * S + S / 10));
  CHECK(hkex_valid_price(10 * S));
  CHECK(!hkex_valid_price(10 * S + S / 100));  // 10.01: above 10 the tick is 0.02
  CHECK(!hkex_valid_price(0));
}

void test_lifecycle_and_position() {
  OrderTable t(small());
  const OrderSlot s = t.new_order(0, OrdSide::Buy, 10 * S, 300 * S);
  CHECK_EQ(s, 0u);
  const std::uint32_t id = t.order(s).req_id;
  CHECK_EQ(id, 10'000'000u);
  CHECK(t.order(s).state == OrdState::PendingNew);
  CHECK_EQ(t.position(0).open_buy_qty, 300 * S);
  CHECK_EQ(t.position(0).open_buy_notional, 3000 * S);
  CHECK_EQ(t.position(0).live_orders, 1u);

  Report ack = rep(ReportKind::Ack, id);
  ack.exchange_order_id = "555";
  CHECK(t.apply(ack).event == OrderEvent::Acked);
  CHECK(t.exchange_id(s) == "555");
  CHECK(t.apply(ack).event == OrderEvent::Duplicate);

  auto u = t.apply(fill(id, 100 * S, 10 * S, 100 * S, 200 * S));
  CHECK(u.event == OrderEvent::Filled);
  CHECK_EQ(u.fill_qty, 100 * S);
  CHECK(t.order(s).state == OrdState::PartiallyFilled);
  CHECK_EQ(t.position(0).pos, 100 * S);
  CHECK_EQ(t.position(0).open_buy_qty, 200 * S);
  // the same fill resent (PossResend) is recognised by cumulative quantity
  CHECK(t.apply(fill(id, 100 * S, 10 * S, 100 * S, 200 * S)).event == OrderEvent::Duplicate);
  CHECK_EQ(t.position(0).pos, 100 * S);

  CHECK(t.apply(fill(id, 200 * S, 10 * S, 300 * S, 0)).event == OrderEvent::Filled);
  CHECK(t.order(s).state == OrdState::Filled);
  CHECK_EQ(t.position(0).pos, 300 * S);
  CHECK_EQ(t.position(0).open_buy_qty, 0);
  CHECK_EQ(t.position(0).live_orders, 0u);
  CHECK_EQ(t.position(0).buy_notional, 3000 * S);

  // sell it back one tick higher: P&L 0.02 * 300 = 6
  const OrderSlot s2 = t.new_order(0, OrdSide::Sell, 10 * S + S / 50, 300 * S);
  CHECK_EQ(t.position(0).open_sell_qty, 300 * S);
  t.apply(fill(t.order(s2).req_id, 300 * S, 10 * S + S / 50, 300 * S, 0));
  CHECK_EQ(t.position(0).pos, 0);
  CHECK_EQ(t.position(0).cash(), 6 * S);
  CHECK_EQ(t.position(0).pnl(123 * S), 6 * S);
}

void test_reject_and_unknown() {
  OrderTable t(small());
  const OrderSlot s = t.new_order(1, OrdSide::Sell, 5 * S, 100 * S);
  Report r = rep(ReportKind::Reject, t.order(s).req_id);
  r.reject_code = 3;
  auto u = t.apply(r);
  CHECK(u.event == OrderEvent::Rejected);
  CHECK_EQ(u.reject_code, 3);
  CHECK(t.order(s).state == OrdState::Rejected);
  CHECK_EQ(t.position(1).open_sell_qty, 0);
  CHECK(t.apply(rep(ReportKind::Ack, 12'345'678)).event == OrderEvent::Unknown);
  CHECK(t.apply(rep(ReportKind::Ack, 5)).event == OrderEvent::Unknown);  // below the first ID
}

void test_cancel_paths() {
  OrderTable t(small());
  // fill during pending cancel, then cancel ack
  OrderSlot s = t.new_order(0, OrdSide::Buy, 10 * S, 300 * S);
  const std::uint32_t id = t.order(s).req_id;
  t.apply(rep(ReportKind::Ack, id));
  const std::uint32_t cid = t.cancel(s);
  CHECK(cid != 0);
  CHECK_EQ(t.cancel(s), 0u);  // already pending
  CHECK(t.kind_of(cid) == ReqKind::Cancel);
  CHECK_EQ(t.slot_of(cid), s);
  t.apply(fill(id, 100 * S, 10 * S, 100 * S, 200 * S));
  Report c = rep(ReportKind::Cancelled, cid);
  c.orig_req_id = id;
  CHECK(t.apply(c).event == OrderEvent::Cancelled);
  CHECK(t.order(s).state == OrdState::Cancelled);
  CHECK_EQ(t.position(0).pos, 100 * S);
  CHECK_EQ(t.position(0).open_buy_qty, 0);
  CHECK_EQ(t.cancel(s), 0u);  // not live

  // cancel rejected (too late) after a full fill
  s = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  const std::uint32_t id2 = t.order(s).req_id;
  const std::uint32_t cid2 = t.cancel(s);
  t.apply(fill(id2, 100 * S, 10 * S, 100 * S, 0));
  CHECK(t.apply(rep(ReportKind::CancelReject, cid2)).event == OrderEvent::Duplicate);
  CHECK(t.order(s).state == OrdState::Filled);

  // cancel rejected while still live: pending cleared, can cancel again
  s = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  t.apply(rep(ReportKind::Ack, t.order(s).req_id));
  const std::uint32_t cid3 = t.cancel(s);
  Report cr = rep(ReportKind::CancelReject, cid3);
  cr.reject_code = 1;
  auto u = t.apply(cr);
  CHECK(u.event == OrderEvent::CancelRejected);
  CHECK_EQ(u.reject_code, 1);
  CHECK(t.order(s).pending == kPendingNone);
  CHECK(t.cancel(s) != 0);

  // unsolicited cancel uses the order's own ID
  s = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  t.apply(rep(ReportKind::Ack, t.order(s).req_id));
  CHECK(t.apply(rep(ReportKind::Cancelled, t.order(s).req_id)).event == OrderEvent::Cancelled);

  // a late fill after the order is gone still moves the position, without corrupting the list
  s = t.new_order(2, OrdSide::Buy, 10 * S, 100 * S);
  const std::uint32_t id5 = t.order(s).req_id;
  t.apply(rep(ReportKind::Ack, id5));
  t.apply(rep(ReportKind::Cancelled, id5));
  CHECK(t.apply(fill(id5, 50 * S, 10 * S, 50 * S, 0)).event == OrderEvent::Filled);
  CHECK_EQ(t.position(2).pos, 50 * S);
  CHECK_EQ(t.position(2).live_orders, 0u);
  CHECK(t.order(s).state == OrdState::Cancelled);
}

void test_amend_paths() {
  OrderTable t(small());
  const OrderSlot s = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  const std::uint32_t id = t.order(s).req_id;
  CHECK_EQ(t.amend(s, 11 * S, 300 * S), 0u);  // not acked yet
  t.apply(rep(ReportKind::Ack, id));

  // amend up in quantity: exposure is the larger of old and new while in flight
  const std::uint32_t aid = t.amend(s, 10 * S, 300 * S);
  CHECK(aid != 0);
  CHECK_EQ(t.position(0).open_buy_qty, 300 * S);
  CHECK_EQ(t.cancel(s), 0u);  // one request in flight at a time
  Report rej = rep(ReportKind::AmendReject, aid);
  CHECK(t.apply(rej).event == OrderEvent::AmendRejected);
  CHECK_EQ(t.position(0).open_buy_qty, 100 * S);

  // amend down in quantity, partial fill in between, then ack
  const std::uint32_t aid2 = t.amend(s, 11 * S, 60 * S);
  CHECK_EQ(t.position(0).open_buy_qty, 100 * S);   // max(100 leaves, 60 new)
  t.apply(fill(id, 20 * S, 10 * S, 20 * S, 80 * S));  // fill under the old ID
  CHECK_EQ(t.position(0).open_buy_qty, 80 * S);    // max(80, 60 - 20)
  Report a = rep(ReportKind::Amended, aid2);
  a.orig_req_id = id;
  a.price = 11 * S;
  a.order_qty = 60 * S;
  a.cum_qty = 20 * S;
  a.leaves_qty = 40 * S;
  CHECK(t.apply(a).event == OrderEvent::Amended);
  CHECK_EQ(t.order(s).price, 11 * S);
  CHECK_EQ(t.order(s).leaves_qty, 40 * S);
  CHECK_EQ(t.order(s).req_id, aid2);
  CHECK_EQ(t.position(0).open_buy_qty, 40 * S);
  CHECK_EQ(t.position(0).open_buy_notional, 440 * S);
  CHECK(t.apply(a).event == OrderEvent::Duplicate);
  // later reports carry the new ID
  CHECK(t.apply(fill(aid2, 40 * S, 11 * S, 60 * S, 0)).event == OrderEvent::Filled);
  CHECK(t.order(s).state == OrdState::Filled);
  CHECK_EQ(t.position(0).pos, 60 * S);
}

void test_trade_cancel_and_unsend() {
  OrderTable t(small());
  const OrderSlot s = t.new_order(0, OrdSide::Sell, 10 * S, 100 * S);
  const std::uint32_t id = t.order(s).req_id;
  t.apply(fill(id, 100 * S, 10 * S, 100 * S, 0));
  CHECK_EQ(t.position(0).pos, -100 * S);
  Report b = rep(ReportKind::TradeCancel, id);
  b.exec_qty = 100 * S;
  b.exec_price = 10 * S;
  CHECK(t.apply(b).event == OrderEvent::FillBusted);
  CHECK_EQ(t.position(0).pos, 0);
  CHECK_EQ(t.position(0).sell_notional, 0);

  // a new order that was gap-filled instead of sent
  const OrderSlot s2 = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  t.unsend(t.order(s2).req_id);
  CHECK(t.order(s2).state == OrdState::Rejected);
  CHECK_EQ(t.position(0).open_buy_qty, 0);
  // a cancel that was never sent
  const OrderSlot s3 = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  t.apply(rep(ReportKind::Ack, t.order(s3).req_id));
  t.unsend(t.cancel(s3));
  CHECK(t.order(s3).pending == kPendingNone);
  CHECK(t.order(s3).state == OrdState::New);
}

// Random operations; after each one the running totals must equal a recount over live orders.
void test_random_consistency() {
  OrderTable t(small());
  std::mt19937 rng(11);
  std::vector<OrderSlot> slots;
  auto check_totals = [&] {
    for (SymbolIdx sym = 0; sym < 3; ++sym) {
      std::int64_t buy = 0, sell = 0, nb = 0, ns = 0;
      std::uint32_t live = 0;
      for (OrderSlot s = 0; s < t.order_count(); ++s) {
        const Order& o = t.order(s);
        if (o.symbol != sym || !is_live(o.state)) continue;
        ++live;
        std::int64_t q = o.leaves_qty, n = notional(o.price, o.leaves_qty);
        if (o.pending & kPendingAmend) {
          q = std::max(q, o.amend_qty - o.cum_qty);
          n = std::max(n, notional(o.amend_price, o.amend_qty - o.cum_qty));
        }
        (is_buy(o.side) ? buy : sell) += q;
        (is_buy(o.side) ? nb : ns) += n;
      }
      std::uint32_t listed = 0;
      t.for_each_live(sym, [&](OrderSlot, const Order& o) {
        ++listed;
        CHECK(is_live(o.state));
      });
      const Position& p = t.position(sym);
      CHECK_EQ(p.open_buy_qty, buy);
      CHECK_EQ(p.open_sell_qty, sell);
      CHECK_EQ(p.open_buy_notional, nb);
      CHECK_EQ(p.open_sell_notional, ns);
      CHECK_EQ(p.live_orders, live);
      CHECK_EQ(listed, live);
    }
  };
  for (int step = 0; step < 20000; ++step) {
    const int op = static_cast<int>(rng() % 10);
    if (op < 3 || slots.empty()) {
      const auto side = static_cast<OrdSide>(rng() % 2);
      slots.push_back(t.new_order(rng() % 3, side, (10 + rng() % 5) * S, (1 + rng() % 5) * 100 * S));
    } else {
      const OrderSlot s = slots[rng() % slots.size()];
      const Order o = t.order(s);
      switch (op) {
        case 3: t.apply(rep(ReportKind::Ack, o.req_id)); break;
        case 4: t.cancel(s); break;
        case 5:
          if (o.pending_req && (o.pending & kPendingCancel)) t.apply(rep(ReportKind::Cancelled, o.pending_req));
          break;
        case 6: t.amend(s, (10 + rng() % 5) * S, o.cum_qty + (1 + rng() % 5) * 100 * S); break;
        case 7:
          if (o.pending & kPendingAmend) {
            Report a = rep(rng() % 2 ? ReportKind::Amended : ReportKind::AmendReject, o.pending_req);
            t.apply(a);
          }
          break;
        case 8:
          if (is_live(o.state) && o.leaves_qty > 0) {
            const std::int64_t q = std::min<std::int64_t>(o.leaves_qty, 100 * S);
            t.apply(fill(o.req_id, q, o.price, o.cum_qty + q, o.leaves_qty - q));
          }
          break;
        case 9:
          if (o.pending_req) t.apply(rep(ReportKind::CancelReject, o.pending_req));
          break;
      }
    }
    if (step % 97 == 0) check_totals();
  }
  check_totals();
}

void test_ocgc_adapter() {
  using namespace obl::gw::ocgc;
  ExecReport er;
  namespace b = exec_report;
  er.present = 1ull << b::ClOrdId | 1ull << b::OrigClOrdId | 1ull << b::ExecType | 1ull << b::CumQty |
               1ull << b::LeavesQty | 1ull << b::Price | 1ull << b::OrderId;
  er.cl_ord_id = "10000005";
  er.orig_cl_ord_id = "10000001";
  er.exec_type = ExecType::Amend;
  er.cum_qty = 0;
  er.leaves_qty = 50 * S;
  er.price = 11 * S;
  er.order_id = "XYZ";
  const Report r = to_report(er);
  CHECK(r.kind == ReportKind::Amended);
  CHECK_EQ(r.req_id, 10000005u);
  CHECK_EQ(r.orig_req_id, 10000001u);
  CHECK_EQ(r.cum_qty, 0);
  CHECK_EQ(r.leaves_qty, 50 * S);
  CHECK_EQ(r.order_qty, 0);  // absent
  CHECK(r.exchange_order_id == "XYZ");
}

}  // namespace

int main() {
  test_tick_table();
  test_lifecycle_and_position();
  test_reject_and_unknown();
  test_cancel_paths();
  test_amend_paths();
  test_trade_cancel_and_unsend();
  test_random_consistency();
  test_ocgc_adapter();
  return test_result("test_order_state");
}
