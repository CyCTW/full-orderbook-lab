#pragma once

// Pre-trade risk checks and the kill switch.
//
// check_new / check_amend run on the send path before anything is encoded. They read the order
// table (positions, open exposure, live orders) and the instrument table (reference price, board
// lot, flags) and return the first failed check. Cancels are never blocked, not even by the kill
// switch: cancelling only reduces risk.
//
// Kill switch: a flag any thread may set (relaxed atomic, one load on the send path). While set,
// every new order and amend is refused; the gateway also cancels everything it has open. It is
// engaged by hand, or automatically when
//   - the account's mark-to-market loss exceeds max_daily_loss
//   - the exchange rejects more than max_rejects_per_window orders within reject_window_ns
//   - the strategy trips the order-rate limit more than max_rate_breaches times
// and stays engaged until reset() is called by a person.

#include <atomic>
#include <cstdint>
#include <vector>

#include "obl/gw/instrument.hpp"
#include "obl/gw/order_state.hpp"
#include "obl/gw/rate_limit.hpp"

namespace obl::gw {

enum class RiskReject : std::uint8_t {
  None,
  KillSwitch,
  Restricted,         // symbol on the restricted list
  UnknownSymbol,
  BadPrice,           // <= 0 or not on the exchange tick grid
  BadQuantity,        // <= 0 or not a multiple of the board lot
  MaxOrderQty,
  MaxOrderNotional,
  PriceCollar,        // too far from the reference price
  NoReferencePrice,   // collar configured but no reference price known
  ShortSellNotAllowed,  // SellShort on a symbol that is not shortable, or short selling disabled
  InsufficientPosition, // plain Sell larger than the long position (not covered by open sells)
  MaxLongPosition,
  MaxShortPosition,
  MaxGrossOpenNotional, // account credit line
  OrderRate,
  Duplicate,
  SelfTrade,          // would cross one of our own resting orders
};

inline const char* to_string(RiskReject r) {
  switch (r) {
    case RiskReject::None: return "ok";
    case RiskReject::KillSwitch: return "kill switch engaged";
    case RiskReject::Restricted: return "restricted symbol";
    case RiskReject::UnknownSymbol: return "unknown symbol";
    case RiskReject::BadPrice: return "invalid price";
    case RiskReject::BadQuantity: return "invalid quantity";
    case RiskReject::MaxOrderQty: return "order quantity limit";
    case RiskReject::MaxOrderNotional: return "order notional limit";
    case RiskReject::PriceCollar: return "price collar";
    case RiskReject::NoReferencePrice: return "no reference price";
    case RiskReject::ShortSellNotAllowed: return "short sell not allowed";
    case RiskReject::InsufficientPosition: return "insufficient position";
    case RiskReject::MaxLongPosition: return "long position limit";
    case RiskReject::MaxShortPosition: return "short position limit";
    case RiskReject::MaxGrossOpenNotional: return "gross open notional limit";
    case RiskReject::OrderRate: return "order rate limit";
    case RiskReject::Duplicate: return "duplicate order";
    case RiskReject::SelfTrade: return "self trade";
  }
  return "?";
}

enum class KillReason : std::uint8_t { None, Manual, DailyLoss, RejectRate, OrderRate };

inline const char* to_string(KillReason r) {
  switch (r) {
    case KillReason::None: return "none";
    case KillReason::Manual: return "manual";
    case KillReason::DailyLoss: return "daily loss limit";
    case KillReason::RejectRate: return "exchange reject rate";
    case KillReason::OrderRate: return "order rate breaches";
  }
  return "?";
}

class KillSwitch {
 public:
  bool engaged() const { return reason_.load(std::memory_order_relaxed) != KillReason::None; }
  KillReason reason() const { return reason_.load(std::memory_order_relaxed); }
  // Returns true if this call engaged it (it was not engaged before).
  bool engage(KillReason r) {
    KillReason expected = KillReason::None;
    return reason_.compare_exchange_strong(expected, r, std::memory_order_relaxed);
  }
  void reset() { reason_.store(KillReason::None, std::memory_order_relaxed); }

 private:
  std::atomic<KillReason> reason_{KillReason::None};
};

// 0 means "no limit" for every field.
struct SymbolLimits {
  std::int64_t max_order_qty = 0;
  std::int64_t max_order_notional = 0;
  std::uint32_t collar_bps = 0;     // max distance from the reference price, basis points
  std::int64_t max_long = 0;        // position + open buys
  std::int64_t max_short = 0;       // -(position - open sells), as a positive number
};

struct AccountLimits {
  std::int64_t max_gross_open_notional = 0;  // sum over symbols of open buy + open sell notional
  std::int64_t max_daily_loss = 0;           // positive number; kill switch when P&L < -this
  std::uint32_t max_orders_per_sec = 0;      // new orders + amends
  std::uint32_t order_burst = 0;
  std::uint32_t max_rate_breaches = 0;       // order-rate rejects before the kill switch (0: never)
  std::uint64_t duplicate_window_ns = 0;     // same symbol/side/price/qty within this -> reject
  bool allow_short_sell = false;
  bool self_trade_check = true;
  std::uint32_t max_rejects_per_window = 0;  // exchange rejects before the kill switch (0: never)
  std::uint64_t reject_window_ns = 1'000'000'000;
};

class RiskEngine {
 public:
  RiskEngine(InstrumentTable& instruments, const OrderTable& orders, std::size_t max_symbols = 4096)
      : ins_(instruments), orders_(orders), sym_(max_symbols) {}

  void set_account_limits(const AccountLimits& a) {
    acct_ = a;
    order_rate_.configure(a.max_orders_per_sec, a.order_burst ? a.order_burst : a.max_orders_per_sec);
    reject_window_.configure(a.max_rejects_per_window, a.reject_window_ns);
  }
  void set_symbol_limits(SymbolIdx s, const SymbolLimits& l) { sym_[s].limits = l; }
  const AccountLimits& account_limits() const { return acct_; }
  KillSwitch& kill_switch() { return kill_; }
  const KillSwitch& kill_switch() const { return kill_; }

  // --- send path ------------------------------------------------------------------------------

  RiskReject check_new(SymbolIdx s, OrdSide side, std::int64_t price, std::int64_t qty, std::uint64_t now) {
    if (kill_.engaged()) [[unlikely]]
      return RiskReject::KillSwitch;
    if (s >= ins_.size()) return RiskReject::UnknownSymbol;
    const Instrument& in = ins_[s];
    const SymState& st = sym_[s];
    const SymbolLimits& L = st.limits;
    const Position& p = orders_.position(s);

    if (in.restricted) return RiskReject::Restricted;
    if (!hkex_valid_price(price)) return RiskReject::BadPrice;
    if (qty <= 0 || (in.board_lot && qty % in.board_lot)) return RiskReject::BadQuantity;
    if (L.max_order_qty && qty > L.max_order_qty) return RiskReject::MaxOrderQty;
    const std::int64_t n = notional(price, qty);
    if (L.max_order_notional && n > L.max_order_notional) return RiskReject::MaxOrderNotional;
    if (const RiskReject r = check_collar(in, L, price); r != RiskReject::None) return r;

    if (side == OrdSide::SellShort) {
      if (!acct_.allow_short_sell || !in.shortable) return RiskReject::ShortSellNotAllowed;
    } else if (side == OrdSide::Sell) {
      // a plain sell must be covered by the long position net of other open sells
      if (p.pos - p.open_sell_qty < qty) return RiskReject::InsufficientPosition;
    }
    if (is_buy(side)) {
      if (L.max_long && p.pos + p.open_buy_qty + qty > L.max_long) return RiskReject::MaxLongPosition;
    } else if (L.max_short && -(p.pos - p.open_sell_qty - qty) > L.max_short) {
      return RiskReject::MaxShortPosition;
    }
    if (acct_.max_gross_open_notional && gross_open_notional() + n > acct_.max_gross_open_notional)
      return RiskReject::MaxGrossOpenNotional;

    if (acct_.duplicate_window_ns && st.has_last && now - st.last_time < acct_.duplicate_window_ns &&
        st.last_side == side && st.last_price == price && st.last_qty == qty)
      return RiskReject::Duplicate;
    if (acct_.self_trade_check && crosses_own(s, side, price)) return RiskReject::SelfTrade;
    if (!order_rate_.try_take(now)) return rate_breach();
    return RiskReject::None;
  }

  // Record an order that passed check_new and is being sent (duplicate detection).
  void on_new_sent(SymbolIdx s, OrdSide side, std::int64_t price, std::int64_t qty, std::uint64_t now) {
    SymState& st = sym_[s];
    st.has_last = true;
    st.last_time = now;
    st.last_side = side;
    st.last_price = price;
    st.last_qty = qty;
  }

  RiskReject check_amend(OrderSlot slot, std::int64_t new_price, std::int64_t new_qty, std::uint64_t now) {
    if (kill_.engaged()) [[unlikely]]
      return RiskReject::KillSwitch;
    const Order& o = orders_.order(slot);
    const Instrument& in = ins_[o.symbol];
    const SymbolLimits& L = sym_[o.symbol].limits;
    const Position& p = orders_.position(o.symbol);
    if (!hkex_valid_price(new_price)) return RiskReject::BadPrice;
    if (new_qty <= o.cum_qty || (in.board_lot && new_qty % in.board_lot)) return RiskReject::BadQuantity;
    if (L.max_order_qty && new_qty > L.max_order_qty) return RiskReject::MaxOrderQty;
    if (L.max_order_notional && notional(new_price, new_qty) > L.max_order_notional) return RiskReject::MaxOrderNotional;
    if (const RiskReject r = check_collar(in, L, new_price); r != RiskReject::None) return r;
    // only the increase in open quantity adds exposure
    const std::int64_t extra = std::max<std::int64_t>(0, (new_qty - o.cum_qty) - o.leaves_qty);
    if (is_buy(o.side)) {
      if (L.max_long && p.pos + p.open_buy_qty + extra > L.max_long) return RiskReject::MaxLongPosition;
    } else {
      if (o.side == OrdSide::Sell && p.pos - p.open_sell_qty < extra) return RiskReject::InsufficientPosition;
      if (L.max_short && -(p.pos - p.open_sell_qty - extra) > L.max_short) return RiskReject::MaxShortPosition;
    }
    if (acct_.max_gross_open_notional) {
      const std::int64_t extra_n =
          std::max<std::int64_t>(0, notional(new_price, new_qty - o.cum_qty) - notional(o.price, o.leaves_qty));
      if (gross_open_notional() + extra_n > acct_.max_gross_open_notional) return RiskReject::MaxGrossOpenNotional;
    }
    if (acct_.self_trade_check && crosses_own(o.symbol, o.side, new_price)) return RiskReject::SelfTrade;
    if (!order_rate_.try_take(now)) return rate_breach();
    return RiskReject::None;
  }

  // --- report path ------------------------------------------------------------------------------

  // After a fill or a reference price change: re-mark the symbol, check the daily loss.
  void on_position_change(SymbolIdx s) {
    const Instrument& in = ins_[s];
    if (!in.reference_price) return;
    SymState& st = sym_[s];
    const std::int64_t pnl = orders_.position(s).pnl(in.reference_price);
    total_pnl_ += pnl - st.pnl;
    st.pnl = pnl;
    if (acct_.max_daily_loss && total_pnl_ < -acct_.max_daily_loss) kill_.engage(KillReason::DailyLoss);
  }

  void set_reference_price(SymbolIdx s, std::int64_t px) {
    ins_[s].reference_price = px;
    on_position_change(s);
  }

  void on_exchange_reject(std::uint64_t now) {
    if (reject_window_.unlimited()) return;
    if (!reject_window_.try_take(now)) kill_.engage(KillReason::RejectRate);
  }

  std::int64_t total_pnl() const { return total_pnl_; }
  std::uint32_t rate_breaches() const { return rate_breaches_; }

  std::int64_t gross_open_notional() const { return orders_.gross_open_notional(); }

 private:
  struct SymState {
    SymbolLimits limits;
    std::int64_t pnl = 0;  // last mark-to-market, for incremental total
    std::uint64_t last_time = 0;
    std::int64_t last_price = 0, last_qty = 0;
    OrdSide last_side = OrdSide::Buy;
    bool has_last = false;
  };

  static RiskReject check_collar(const Instrument& in, const SymbolLimits& L, std::int64_t price) {
    if (!L.collar_bps) return RiskReject::None;
    if (!in.reference_price) return RiskReject::NoReferencePrice;
    const std::int64_t diff = price > in.reference_price ? price - in.reference_price : in.reference_price - price;
    // diff / ref > bps / 10000  <=>  diff * 10000 > bps * ref
    if (static_cast<int128>(diff) * 10'000 > static_cast<int128>(L.collar_bps) * in.reference_price)
      return RiskReject::PriceCollar;
    return RiskReject::None;
  }

  bool crosses_own(SymbolIdx s, OrdSide side, std::int64_t price) const {
    bool cross = false;
    orders_.for_each_live(s, [&](OrderSlot, const Order& o) {
      if (is_buy(o.side) == is_buy(side)) return;
      if (is_buy(side) ? o.price <= price : o.price >= price) cross = true;
    });
    return cross;
  }

  RiskReject rate_breach() {
    ++rate_breaches_;
    if (acct_.max_rate_breaches && rate_breaches_ >= acct_.max_rate_breaches) kill_.engage(KillReason::OrderRate);
    return RiskReject::OrderRate;
  }

  InstrumentTable& ins_;
  const OrderTable& orders_;
  std::vector<SymState> sym_;
  AccountLimits acct_;
  KillSwitch kill_;
  Gcra order_rate_;
  SlidingWindow reject_window_;
  std::int64_t total_pnl_ = 0;
  std::uint32_t rate_breaches_ = 0;
};

}  // namespace obl::gw
