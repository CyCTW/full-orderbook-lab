#pragma once

// Own-order state: every order this gateway sent today, its in-flight requests, and per-symbol
// position and open exposure.
//
// IDs. Every request (new, cancel, amend, mass cancel) carries a fresh request ID (OCG-C: Client
// Order ID, unique per day, §6.6.3.1). The gateway allocates them densely from `first_req_id`, so
// `req_id - first_req_id` indexes an array that maps straight to the order slot: no hashing on
// either the send or the report path. Order slots are allocated in sequence and never reused
// within a day, so a slot number given to the strategy stays valid.
//
// Layout. The fields touched on every send and every report sit in one 64-byte Order; the
// exchange Order ID, the strategy's tag and the per-symbol live list are in separate cold arrays.
//
// Exposure. For each live order the worst case open quantity is
//   leaves                            normally
//   max(leaves, amended leaves)       while an amend is in flight (it may go either way)
// and every transition updates the per-symbol totals by (after - before), so open buy/sell
// quantity and notional are always the sum over live orders without ever rescanning them.
//
// Duplicates. Reports resent with PossResend may arrive twice under different sequence numbers
// (OCG-C §5.5.2). Fills are recognised as duplicates by cumulative quantity (a fill whose
// CumQty is not above what the order already has is old); other report kinds are idempotent.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "obl/gw/instrument.hpp"

namespace obl::gw {

using OrderSlot = std::uint32_t;
inline constexpr OrderSlot kNoOrder = 0xFFFFFFFFu;

enum class OrdSide : std::uint8_t { Buy = 0, Sell = 1, SellShort = 2 };
inline bool is_buy(OrdSide s) { return s == OrdSide::Buy; }

enum class OrdState : std::uint8_t {
  PendingNew,       // sent, no ack yet
  New,              // live, nothing filled
  PartiallyFilled,  // live
  Filled,           // terminal
  Cancelled,        // terminal
  Rejected,         // terminal
  Expired,          // terminal
};

inline bool is_live(OrdState s) {
  return s == OrdState::PendingNew || s == OrdState::New || s == OrdState::PartiallyFilled;
}

enum class OrdTif : std::uint8_t { Day, IOC, FOK, AtCrossing };

enum PendingFlags : std::uint8_t {
  kPendingNone = 0,
  kPendingCancel = 1,
  kPendingAmend = 2,
  kUnsent = 4,  // the latest request (new, cancel or amend) is queued locally, not on the wire yet
};

struct alignas(64) Order {
  std::int64_t price = 0;       // current (acknowledged) limit price
  std::int64_t qty = 0;         // current total order quantity
  std::int64_t cum_qty = 0;
  std::int64_t leaves_qty = 0;
  std::int64_t amend_price = 0;  // in-flight amend: new price / new total quantity
  std::int64_t amend_qty = 0;
  SymbolIdx symbol = kNoSymbol;
  std::uint32_t req_id = 0;     // request ID the exchange knows the order by (changes on amend)
  std::uint32_t pending_req = 0;  // in-flight cancel / amend request ID
  OrdState state = OrdState::PendingNew;
  OrdSide side = OrdSide::Buy;
  std::uint8_t pending = kPendingNone;
  OrdTif tif = OrdTif::Day;
};
static_assert(sizeof(Order) == 64);

struct Position {
  std::int64_t pos = 0;            // signed shares (fixed point)
  std::int64_t bought = 0, sold = 0;
  std::int64_t buy_notional = 0, sell_notional = 0;
  std::int64_t open_buy_qty = 0, open_sell_qty = 0;  // worst case over live orders
  std::int64_t open_buy_notional = 0, open_sell_notional = 0;
  std::uint32_t live_orders = 0;

  std::int64_t cash() const { return sell_notional - buy_notional; }
  // Mark-to-market P&L at `mark` (fees not included).
  std::int64_t pnl(std::int64_t mark) const { return cash() + notional(mark, pos); }
};

// Report from the exchange, protocol neutral (the OCG-C adapter fills it from an Execution Report).
enum class ReportKind : std::uint8_t {
  Ack,           // new order accepted
  Reject,        // new order rejected
  Fill,          // (partial) execution
  Cancelled,     // solicited or unsolicited
  Expired,
  Amended,
  CancelReject,
  AmendReject,
  TradeCancel,   // execution busted by the exchange
  Other,
};

struct Report {
  ReportKind kind = ReportKind::Other;
  std::uint32_t req_id = 0;       // Client Order ID of the report
  std::uint32_t orig_req_id = 0;  // Original Client Order ID, if any
  std::int64_t price = 0, order_qty = 0;  // order's price / total quantity, if present
  std::int64_t cum_qty = -1, leaves_qty = -1;  // -1 = not present
  std::int64_t exec_qty = 0, exec_price = 0;
  std::uint16_t reject_code = 0;
  std::string_view exchange_order_id;
};

// What applying a report did, for the strategy.
enum class OrderEvent : std::uint8_t {
  Acked,
  Rejected,
  Filled,          // partial or full; see fill_qty and Order::leaves_qty
  Cancelled,
  Expired,
  Amended,
  CancelRejected,
  AmendRejected,
  FillBusted,
  Duplicate,       // already applied (resent report)
  Unknown,         // request ID we never sent
};

struct OrderUpdate {
  OrderEvent event;
  OrderSlot slot = kNoOrder;
  std::int64_t fill_qty = 0, fill_price = 0;
  std::uint16_t reject_code = 0;
};

enum class ReqKind : std::uint8_t { None, New, Cancel, Amend, MassCancel };

struct OrderTableConfig {
  std::uint32_t first_req_id = 10'000'000;  // 8-digit OCG-C Client Order IDs
  std::uint32_t max_requests = 1u << 22;     // per day
  std::uint32_t max_orders = 1u << 20;       // per day
  std::uint32_t max_symbols = 4096;
};

class OrderTable {
 public:
  explicit OrderTable(const OrderTableConfig& cfg = {})
      : cfg_(cfg), next_req_(cfg.first_req_id) {
    // sized (and therefore touched) up front: no allocation or page fault on the send path
    reqs_.resize(cfg.max_requests);
    orders_.resize(cfg.max_orders);
    cold_.resize(cfg.max_orders);
    positions_.resize(cfg.max_symbols);
    live_head_.assign(cfg.max_symbols, kNoOrder);
  }

  // --- requests (called when the message is sent) -------------------------------------------

  // Allocates a slot and request ID for a new order. Returns kNoOrder when out of IDs or slots.
  OrderSlot new_order(SymbolIdx sym, OrdSide side, std::int64_t price, std::int64_t qty,
                      OrdTif tif = OrdTif::Day, std::uint64_t tag = 0) {
    if (n_orders_ == cfg_.max_orders || !have_req()) [[unlikely]]
      return kNoOrder;
    const OrderSlot s = n_orders_++;
    Order& o = orders_[s];
    o = Order{};
    o.symbol = sym;
    o.side = side;
    o.price = price;
    o.qty = qty;
    o.leaves_qty = qty;
    o.tif = tif;
    o.req_id = alloc_req(ReqKind::New, s);
    cold_[s] = Cold{};
    cold_[s].tag = tag;
    link(s);
    add_exposure(o, +1);
    return s;
  }

  // Cancel request for a live order with no cancel or amend in flight (OCG-C rejects a cancel
  // while either is pending: Cancel Reject Code 3). Returns the request ID, or 0 if the order
  // cannot be cancelled now.
  std::uint32_t cancel(OrderSlot s) {
    Order& o = orders_[s];
    if (!is_live(o.state) || o.pending || !have_req()) return 0;
    o.pending |= kPendingCancel;
    o.pending_req = alloc_req(ReqKind::Cancel, s);
    return o.pending_req;
  }

  // Amend request: new price and new total order quantity (OCG-C §7.6.3; quantity can only go
  // down without losing priority). Returns the request ID, or 0 if not possible now.
  std::uint32_t amend(OrderSlot s, std::int64_t new_price, std::int64_t new_qty) {
    Order& o = orders_[s];
    if (!is_live(o.state) || o.state == OrdState::PendingNew || o.pending || new_qty <= o.cum_qty || !have_req())
      return 0;
    remove_exposure(o);
    o.pending |= kPendingAmend;
    o.amend_price = new_price;
    o.amend_qty = new_qty;
    o.pending_req = alloc_req(ReqKind::Amend, s);
    add_exposure(o, +1);
    return o.pending_req;
  }

  // An amend still queued locally (kUnsent) can be retargeted instead of sending a second one:
  // only the latest price / quantity goes out.
  bool retarget_amend(OrderSlot s, std::int64_t new_price, std::int64_t new_qty) {
    Order& o = orders_[s];
    if (!(o.pending & kPendingAmend) || !(o.pending & kUnsent) || new_qty <= o.cum_qty) return false;
    remove_exposure(o);
    o.amend_price = new_price;
    o.amend_qty = new_qty;
    add_exposure(o, +1);
    return true;
  }

  void mark_unsent(OrderSlot s) { orders_[s].pending |= kUnsent; }
  void mark_sent(OrderSlot s) { orders_[s].pending &= static_cast<std::uint8_t>(~kUnsent); }
  bool is_unsent(OrderSlot s) const { return orders_[s].pending & kUnsent; }

  // Request ID for a mass cancel (not tied to an order).
  std::uint32_t mass_cancel_id() { return have_req() ? alloc_req(ReqKind::MassCancel, kNoOrder) : 0; }

  // A request that was never sent (local reject after allocation, or gap-filled on replay):
  // undo its effect. For a new order the order becomes Rejected.
  void unsend(std::uint32_t req_id) {
    const Req r = lookup(req_id);
    if (r.kind == ReqKind::None || r.slot == kNoOrder) return;
    Order& o = orders_[r.slot];
    if (r.kind == ReqKind::New && o.req_id == req_id) {
      finish(r.slot, OrdState::Rejected);
    } else if (o.pending_req == req_id) {
      remove_exposure(o);
      o.pending = kPendingNone;
      o.pending_req = 0;
      add_exposure(o, +1);
    }
  }

  // --- reports ------------------------------------------------------------------------------

  OrderUpdate apply(const Report& r) {
    Req q = lookup(r.req_id);
    if (q.slot == kNoOrder && r.orig_req_id) q = lookup(r.orig_req_id);
    if (q.kind == ReqKind::None || q.slot == kNoOrder) return {OrderEvent::Unknown};
    const OrderSlot s = q.slot;
    Order& o = orders_[s];
    Position& p = positions_[o.symbol];

    switch (r.kind) {
      case ReportKind::Ack:
        if (o.state != OrdState::PendingNew) return {OrderEvent::Duplicate, s};
        o.state = OrdState::New;
        set_exchange_id(s, r.exchange_order_id);
        return {OrderEvent::Acked, s};

      case ReportKind::Reject:
        if (!is_live(o.state)) return {OrderEvent::Duplicate, s};
        finish(s, OrdState::Rejected);
        return {OrderEvent::Rejected, s, 0, 0, r.reject_code};

      case ReportKind::Fill: {
        const std::int64_t cum = r.cum_qty >= 0 ? r.cum_qty : o.cum_qty + r.exec_qty;
        if (cum <= o.cum_qty) return {OrderEvent::Duplicate, s};
        const std::int64_t q_exec = cum - o.cum_qty;
        book_fill(p, o.side, q_exec, r.exec_price, +1);  // a fill is real money whatever our state says
        if (!is_live(o.state)) {  // late fill on an order we consider gone
          o.cum_qty = cum;
          return {OrderEvent::Filled, s, q_exec, r.exec_price};
        }
        remove_exposure(o);  // before cum_qty changes: exposure of a pending amend depends on it
        o.cum_qty = cum;
        o.leaves_qty = r.leaves_qty >= 0 ? r.leaves_qty : std::max<std::int64_t>(0, o.qty - cum);
        if (o.state == OrdState::PendingNew) set_exchange_id(s, r.exchange_order_id);
        if (o.leaves_qty == 0) {
          o.pending = kPendingNone;  // any in-flight cancel / amend will be rejected (too late)
          o.pending_req = 0;
          o.state = OrdState::Filled;
          unlink(s);
        } else {
          o.state = OrdState::PartiallyFilled;
          add_exposure(o, +1);
        }
        return {OrderEvent::Filled, s, q_exec, r.exec_price};
      }

      case ReportKind::Cancelled:
        if (!is_live(o.state)) return {OrderEvent::Duplicate, s};
        finish(s, OrdState::Cancelled);
        return {OrderEvent::Cancelled, s};

      case ReportKind::Expired:
        if (!is_live(o.state)) return {OrderEvent::Duplicate, s};
        finish(s, OrdState::Expired);
        return {OrderEvent::Expired, s};

      case ReportKind::Amended: {
        if (!(o.pending & kPendingAmend) || r.req_id != o.pending_req) return {OrderEvent::Duplicate, s};
        remove_exposure(o);
        o.price = r.price ? r.price : o.amend_price;
        o.qty = r.order_qty ? r.order_qty : o.amend_qty;
        if (r.cum_qty >= 0) o.cum_qty = std::max(o.cum_qty, r.cum_qty);
        o.leaves_qty = r.leaves_qty >= 0 ? r.leaves_qty : std::max<std::int64_t>(0, o.qty - o.cum_qty);
        o.req_id = o.pending_req;  // the order is now known by the amend's ID
        o.pending &= static_cast<std::uint8_t>(~kPendingAmend);
        o.pending_req = 0;
        add_exposure(o, +1);
        return {OrderEvent::Amended, s};
      }

      case ReportKind::CancelReject:
        if (!(o.pending & kPendingCancel) || r.req_id != o.pending_req) return {OrderEvent::Duplicate, s};
        o.pending &= static_cast<std::uint8_t>(~kPendingCancel);
        o.pending_req = 0;
        return {OrderEvent::CancelRejected, s, 0, 0, r.reject_code};

      case ReportKind::AmendReject:
        if (!(o.pending & kPendingAmend) || r.req_id != o.pending_req) return {OrderEvent::Duplicate, s};
        remove_exposure(o);
        o.pending &= static_cast<std::uint8_t>(~kPendingAmend);
        o.pending_req = 0;
        if (is_live(o.state)) add_exposure(o, +1);
        return {OrderEvent::AmendRejected, s, 0, 0, r.reject_code};

      case ReportKind::TradeCancel: {
        // The exchange reverses an execution: position and cumulative quantity go back. The
        // order's remaining state follows the report (a busted fill does not revive the order
        // unless the exchange says so through Leaves Quantity).
        const bool live = is_live(o.state);
        if (live) remove_exposure(o);
        o.cum_qty = std::max<std::int64_t>(0, o.cum_qty - r.exec_qty);
        book_fill(p, o.side, r.exec_qty, r.exec_price, -1);
        if (live) {
          if (r.leaves_qty >= 0) o.leaves_qty = r.leaves_qty;
          add_exposure(o, +1);
        }
        return {OrderEvent::FillBusted, s, r.exec_qty, r.exec_price};
      }

      case ReportKind::Other:
        break;
    }
    return {OrderEvent::Duplicate, s};
  }

  // --- queries ------------------------------------------------------------------------------

  const Order& order(OrderSlot s) const { return orders_[s]; }
  std::uint64_t tag(OrderSlot s) const { return cold_[s].tag; }
  std::uint8_t route(OrderSlot s) const { return cold_[s].route; }
  void set_route(OrderSlot s, std::uint8_t r) { cold_[s].route = r; }
  std::string_view exchange_id(OrderSlot s) const { return {cold_[s].exch_id, cold_[s].exch_id_len}; }
  const Position& position(SymbolIdx sym) const { return positions_[sym]; }
  // Sum over all symbols of open buy + open sell notional (worst case), kept incrementally.
  std::int64_t gross_open_notional() const { return gross_open_notional_; }
  std::uint32_t order_count() const { return n_orders_; }
  std::uint32_t next_req_id() const { return next_req_; }

  OrderSlot slot_of(std::uint32_t req_id) const { return lookup(req_id).slot; }
  ReqKind kind_of(std::uint32_t req_id) const { return lookup(req_id).kind; }

  // Live orders of a symbol, most recent first.
  template <class F>
  void for_each_live(SymbolIdx sym, F&& f) const {
    for (OrderSlot s = live_head_[sym]; s != kNoOrder;) {
      const OrderSlot next = cold_[s].next;  // f may finish the order
      f(s, orders_[s]);
      s = next;
    }
  }

  // Start of a new trading day: forget everything, keep positions only if asked.
  void reset_day(bool keep_positions) {
    for (std::uint32_t i = 0; i < next_req_ - cfg_.first_req_id; ++i) reqs_[i] = Req{};
    next_req_ = cfg_.first_req_id;
    n_orders_ = 0;
    gross_open_notional_ = 0;
    live_head_.assign(cfg_.max_symbols, kNoOrder);
    for (auto& p : positions_) {
      const std::int64_t pos = p.pos;
      p = Position{};
      if (keep_positions) p.pos = pos;
    }
  }

 private:
  struct Req {
    OrderSlot slot = kNoOrder;
    ReqKind kind = ReqKind::None;
  };
  struct Cold {
    std::uint64_t tag = 0;
    OrderSlot next = kNoOrder, prev = kNoOrder;  // per-symbol live list
    std::uint8_t route = 0;                      // which exchange session carries the order
    std::uint8_t exch_id_len = 0;
    char exch_id[22] = {};
  };

  bool have_req() const { return next_req_ - cfg_.first_req_id < cfg_.max_requests; }

  std::uint32_t alloc_req(ReqKind k, OrderSlot s) {
    reqs_[next_req_ - cfg_.first_req_id] = {s, k};
    return next_req_++;
  }

  Req lookup(std::uint32_t id) const {
    const std::uint32_t i = id - cfg_.first_req_id;  // ids below first wrap to huge values
    return i < next_req_ - cfg_.first_req_id ? reqs_[i] : Req{};
  }

  void set_exchange_id(OrderSlot s, std::string_view id) {
    Cold& c = cold_[s];
    c.exch_id_len = static_cast<std::uint8_t>(std::min(id.size(), sizeof c.exch_id));
    if (c.exch_id_len) std::memcpy(c.exch_id, id.data(), c.exch_id_len);
  }

  static std::int64_t exposure_qty(const Order& o) {
    return (o.pending & kPendingAmend) ? std::max(o.leaves_qty, o.amend_qty - o.cum_qty) : o.leaves_qty;
  }
  static std::int64_t exposure_notional(const Order& o) {
    std::int64_t n = notional(o.price, o.leaves_qty);
    if (o.pending & kPendingAmend) n = std::max(n, notional(o.amend_price, o.amend_qty - o.cum_qty));
    return n;
  }
  void add_exposure(const Order& o, int sign) {
    Position& p = positions_[o.symbol];
    const std::int64_t n = sign * exposure_notional(o);
    gross_open_notional_ += n;
    if (is_buy(o.side)) {
      p.open_buy_qty += sign * exposure_qty(o);
      p.open_buy_notional += n;
    } else {
      p.open_sell_qty += sign * exposure_qty(o);
      p.open_sell_notional += n;
    }
  }
  void remove_exposure(const Order& o) { add_exposure(o, -1); }

  static void book_fill(Position& p, OrdSide side, std::int64_t qty, std::int64_t px, int sign) {
    const std::int64_t n = notional(px, qty);
    if (is_buy(side)) {
      p.pos += sign * qty;
      p.bought += sign * qty;
      p.buy_notional += sign * n;
    } else {
      p.pos -= sign * qty;
      p.sold += sign * qty;
      p.sell_notional += sign * n;
    }
  }

  // Live -> terminal.
  void finish(OrderSlot s, OrdState st) {
    Order& o = orders_[s];
    if (!is_live(o.state)) return;
    remove_exposure(o);
    o.state = st;
    o.leaves_qty = 0;
    o.pending = kPendingNone;
    o.pending_req = 0;
    unlink(s);
  }

  void link(OrderSlot s) {
    const SymbolIdx sym = orders_[s].symbol;
    Cold& c = cold_[s];
    c.prev = kNoOrder;
    c.next = live_head_[sym];
    if (c.next != kNoOrder) cold_[c.next].prev = s;
    live_head_[sym] = s;
    ++positions_[sym].live_orders;
  }
  void unlink(OrderSlot s) {
    const SymbolIdx sym = orders_[s].symbol;
    Cold& c = cold_[s];
    if (c.prev != kNoOrder) cold_[c.prev].next = c.next;
    else live_head_[sym] = c.next;
    if (c.next != kNoOrder) cold_[c.next].prev = c.prev;
    c.next = c.prev = kNoOrder;
    --positions_[sym].live_orders;
  }

  OrderTableConfig cfg_;
  std::uint32_t next_req_;
  std::uint32_t n_orders_ = 0;
  std::int64_t gross_open_notional_ = 0;
  std::vector<Req> reqs_;
  std::vector<Order> orders_;
  std::vector<Cold> cold_;
  std::vector<Position> positions_;
  std::vector<OrderSlot> live_head_;
};

}  // namespace obl::gw
