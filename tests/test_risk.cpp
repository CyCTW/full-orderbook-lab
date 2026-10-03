// Pre-trade risk checks, kill switch triggers, rate limiters.

#include <cstdint>

#include "check.hpp"
#include "obl/gw/rate_limit.hpp"
#include "obl/gw/risk.hpp"

using namespace obl::gw;

namespace {

constexpr std::int64_t S = kScale;
constexpr std::uint64_t kMs = 1'000'000;

struct Fixture {
  InstrumentTable ins;
  OrderTable orders;
  RiskEngine risk;
  SymbolIdx tencent, hsbc;

  static OrderTableConfig cfg() {
    OrderTableConfig c;
    c.max_requests = 1 << 14;
    c.max_orders = 1 << 12;
    c.max_symbols = 8;
    return c;
  }
  Fixture() : orders(cfg()), risk(ins, orders, 8) {
    tencent = ins.add({"700", 100 * S, true, false, 400 * S});
    hsbc = ins.add({"5", 400 * S, false, false, 60 * S});
  }
  // send + fill completely
  void fill_order(SymbolIdx s, OrdSide side, std::int64_t px, std::int64_t qty) {
    const OrderSlot o = orders.new_order(s, side, px, qty);
    Report r;
    r.kind = ReportKind::Fill;
    r.req_id = orders.order(o).req_id;
    r.exec_qty = qty;
    r.exec_price = px;
    r.cum_qty = qty;
    r.leaves_qty = 0;
    orders.apply(r);
    risk.on_position_change(s);
  }
};

void test_gcra_and_window() {
  Gcra g(10, 3);  // 10/s, burst 3
  CHECK(g.try_take(0) && g.try_take(0) && g.try_take(0));
  CHECK(!g.try_take(0));
  CHECK(!g.try_take(99 * kMs));
  CHECK(g.try_take(100 * kMs));  // one interval later
  CHECK(!g.try_take(100 * kMs));
  Gcra unlimited;
  for (int i = 0; i < 100; ++i) CHECK(unlimited.try_take(0));

  SlidingWindow w(3, 1000 * kMs);
  CHECK(w.try_take(0) && w.try_take(10 * kMs) && w.try_take(20 * kMs));
  CHECK(!w.allows(999 * kMs));
  CHECK_EQ(w.next_allowed(500 * kMs), 1000 * kMs);
  CHECK_EQ(w.used(500 * kMs), 3u);
  CHECK(w.try_take(1000 * kMs));  // the first one left the window
  CHECK(!w.try_take(1005 * kMs));
  CHECK(w.try_take(1010 * kMs));
  CHECK_EQ(w.used(1015 * kMs), 3u);
  CHECK_EQ(w.used(5000 * kMs), 0u);
}

void test_order_checks() {
  Fixture f;
  auto& r = f.risk;
  const auto T = f.tencent;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::None);
  CHECK(r.check_new(99, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::UnknownSymbol);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S + S / 10, 100 * S, 0) == RiskReject::BadPrice);  // tick is 0.2
  CHECK(r.check_new(T, OrdSide::Buy, 0, 100 * S, 0) == RiskReject::BadPrice);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 150 * S, 0) == RiskReject::BadQuantity);  // board lot 100
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 0, 0) == RiskReject::BadQuantity);

  SymbolLimits L;
  L.max_order_qty = 1000 * S;
  L.max_order_notional = 200'000 * S;
  L.collar_bps = 500;  // 5 %
  L.max_long = 1500 * S;
  L.max_short = 500 * S;
  r.set_symbol_limits(T, L);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 1100 * S, 0) == RiskReject::MaxOrderQty);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 600 * S, 0) == RiskReject::MaxOrderNotional);  // 240k
  CHECK(r.check_new(T, OrdSide::Buy, 420 * S, 100 * S, 0) == RiskReject::None);   // exactly 5 %
  CHECK(r.check_new(T, OrdSide::Buy, 420 * S + S / 5, 100 * S, 0) == RiskReject::PriceCollar);
  CHECK(r.check_new(T, OrdSide::Buy, 379 * S, 100 * S, 0) == RiskReject::PriceCollar);
  f.ins[T].reference_price = 0;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::NoReferencePrice);
  f.ins[T].reference_price = 400 * S;

  f.ins[T].phase = MarketPhase::Auction;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::MarketPhase);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0, OrdTif::AtCrossing) == RiskReject::None);
  f.ins[T].phase = MarketPhase::Continuous;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0, OrdTif::AtCrossing) == RiskReject::MarketPhase);
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0, OrdTif::IOC) == RiskReject::None);
  f.ins[T].phase = MarketPhase::Halted;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::MarketPhase);
  f.ins[T].phase = MarketPhase::Continuous;

  f.ins[T].restricted = true;
  CHECK(r.check_new(T, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::Restricted);
  f.ins[T].restricted = false;

  // position limits count open orders: 1000 filled + 400 open, a 200 more breaks 1500
  f.fill_order(T, OrdSide::Buy, 400 * S, 1000 * S);
  f.orders.new_order(T, OrdSide::Buy, 398 * S, 400 * S);
  CHECK(r.check_new(T, OrdSide::Buy, 398 * S, 100 * S, 0) == RiskReject::None);
  CHECK(r.check_new(T, OrdSide::Buy, 398 * S, 200 * S, 0) == RiskReject::MaxLongPosition);

  // plain sell must be covered by the long position net of open sells (1000 long)
  const OrderSlot ask = f.orders.new_order(T, OrdSide::Sell, 402 * S, 700 * S);
  CHECK(r.check_new(T, OrdSide::Sell, 404 * S, 300 * S, 0) == RiskReject::None);
  CHECK(r.check_new(T, OrdSide::Sell, 404 * S, 400 * S, 0) == RiskReject::InsufficientPosition);

  // short selling: account switch and per-symbol shortable flag
  CHECK(r.check_new(T, OrdSide::SellShort, 404 * S, 100 * S, 0) == RiskReject::ShortSellNotAllowed);
  AccountLimits A;
  A.allow_short_sell = true;
  r.set_account_limits(A);
  CHECK(r.check_new(T, OrdSide::SellShort, 404 * S, 100 * S, 0) == RiskReject::None);
  CHECK(r.check_new(f.hsbc, OrdSide::SellShort, 60 * S, 400 * S, 0) == RiskReject::ShortSellNotAllowed);
  // short limit 100: pos 1000 - open sells 700 - 400 = -100 ok, -200 not
  L.max_short = 100 * S;
  r.set_symbol_limits(T, L);
  CHECK(r.check_new(T, OrdSide::SellShort, 400 * S, 400 * S, 0) == RiskReject::None);
  CHECK(r.check_new(T, OrdSide::SellShort, 400 * S, 500 * S, 0) == RiskReject::MaxShortPosition);

  // self trade: our ask at 402 rests; a buy at 402 or above would hit it
  CHECK(r.check_new(T, OrdSide::Buy, 402 * S, 100 * S, 0) == RiskReject::SelfTrade);
  CHECK(r.check_new(T, OrdSide::Buy, 401 * S + S * 4 / 5, 100 * S, 0) == RiskReject::None);
  (void)ask;

  // amend: increase beyond the long limit; price move into own ask
  const OrderSlot bid = f.orders.new_order(T, OrdSide::Buy, 396 * S, 100 * S);
  CHECK(r.check_amend(bid, 396 * S, 100 * S, 0) == RiskReject::None);
  CHECK(r.check_amend(bid, 396 * S, 200 * S, 0) == RiskReject::MaxLongPosition);  // 1000 + 500 open + 100
  CHECK(r.check_amend(bid, 402 * S, 100 * S, 0) == RiskReject::SelfTrade);
  CHECK(r.check_amend(bid, 396 * S + S / 10, 100 * S, 0) == RiskReject::BadPrice);
}

void test_account_checks() {
  Fixture f;
  auto& r = f.risk;
  AccountLimits A;
  A.max_gross_open_notional = 100'000 * S;
  A.duplicate_window_ns = 50 * kMs;
  A.max_orders_per_sec = 100;
  A.order_burst = 2;
  A.max_rate_breaches = 3;
  r.set_account_limits(A);
  const auto H = f.hsbc;

  // gross open notional across symbols
  f.orders.new_order(f.tencent, OrdSide::Buy, 400 * S, 200 * S);  // 80k
  CHECK(r.check_new(H, OrdSide::Buy, 50 * S, 400 * S, 0) == RiskReject::None);       // +20k = 100k
  CHECK(r.check_new(H, OrdSide::Buy, 50 * S, 800 * S, 20 * kMs) == RiskReject::MaxGrossOpenNotional);

  // duplicate within the window
  r.on_new_sent(H, OrdSide::Buy, 50 * S, 400 * S, 0);
  CHECK(r.check_new(H, OrdSide::Buy, 50 * S, 400 * S, 40 * kMs) == RiskReject::Duplicate);
  CHECK(r.check_new(H, OrdSide::Buy, 50 * S, 400 * S, 60 * kMs) != RiskReject::Duplicate);

  // order rate: burst 2 already used by the two passing checks above -> breaches, then kill
  r.set_account_limits(A);  // fresh bucket
  const std::uint64_t t0 = 1000 * kMs;
  CHECK(r.check_new(H, OrdSide::Buy, 49 * S, 400 * S, t0) == RiskReject::None);
  CHECK(r.check_new(H, OrdSide::Buy, 48 * S, 400 * S, t0) == RiskReject::None);
  CHECK(r.check_new(H, OrdSide::Buy, 47 * S, 400 * S, t0) == RiskReject::OrderRate);
  CHECK(r.check_new(H, OrdSide::Buy, 47 * S, 400 * S, t0) == RiskReject::OrderRate);
  CHECK(!r.kill_switch().engaged());
  CHECK(r.check_new(H, OrdSide::Buy, 47 * S, 400 * S, t0) == RiskReject::OrderRate);
  CHECK(r.kill_switch().engaged());
  CHECK(r.kill_switch().reason() == KillReason::OrderRate);
  CHECK(r.check_new(H, OrdSide::Buy, 47 * S, 400 * S, t0 + 1000 * kMs) == RiskReject::KillSwitch);
  r.kill_switch().reset();
  CHECK(r.check_new(H, OrdSide::Buy, 47 * S, 400 * S, t0 + 1000 * kMs) == RiskReject::None);
}

void test_kill_switch_triggers() {
  {
    Fixture f;
    AccountLimits A;
    A.max_daily_loss = 1000 * S;
    f.risk.set_account_limits(A);
    f.fill_order(f.tencent, OrdSide::Buy, 400 * S, 100 * S);
    CHECK_EQ(f.risk.total_pnl(), 0);
    f.risk.set_reference_price(f.tencent, 395 * S);  // -500
    CHECK_EQ(f.risk.total_pnl(), -500 * S);
    CHECK(!f.risk.kill_switch().engaged());
    f.risk.set_reference_price(f.tencent, 389 * S);  // -1100
    CHECK(f.risk.kill_switch().engaged());
    CHECK(f.risk.kill_switch().reason() == KillReason::DailyLoss);
    // a second trigger does not overwrite the first reason
    CHECK(!f.risk.kill_switch().engage(KillReason::Manual));
  }
  {
    Fixture f;
    AccountLimits A;
    A.max_rejects_per_window = 3;
    A.reject_window_ns = 1000 * kMs;
    f.risk.set_account_limits(A);
    f.risk.on_exchange_reject(0);
    f.risk.on_exchange_reject(1);
    f.risk.on_exchange_reject(2);
    CHECK(!f.risk.kill_switch().engaged());
    f.risk.on_exchange_reject(3);
    CHECK(f.risk.kill_switch().reason() == KillReason::RejectRate);
  }
  {
    Fixture f;
    CHECK(f.risk.kill_switch().engage(KillReason::Manual));
    CHECK(f.risk.check_new(f.tencent, OrdSide::Buy, 400 * S, 100 * S, 0) == RiskReject::KillSwitch);
  }
}

}  // namespace

int main() {
  test_gcra_and_window();
  test_order_checks();
  test_account_checks();
  test_kill_switch_triggers();
  return test_result("test_risk");
}
