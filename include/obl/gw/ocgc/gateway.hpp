#pragma once

// One OCG-C session's order gateway: strategy requests in, OCG-C messages out, reports back.
//
//   new_order / cancel / amend / mass_cancel   (strategy thread = gateway thread)
//      -> session up?  -> risk -> order table (IDs, exposure) -> throttle -> encode -> session
//   on_bytes (from the transport)
//      -> session (framing, sequence, recovery) -> Execution Report -> order table -> risk
//         (P&L, reject rate) -> Listener
//   poll(now)  heartbeats, throttle queue, expiry of queued orders, kill switch
//
// Shared between several gateways (one per session, e.g. to spread orders over Comp IDs):
// InstrumentTable, OrderTable and RiskEngine. Each order records the gateway (route) it was sent
// on; its cancels and amends go out on the same one.
//
// Time: `now` is UTC nanoseconds since the epoch (CLOCK_REALTIME). The session only uses
// differences; Transaction Time needs the wall clock.
//
// Listener (the strategy side), all called on the gateway thread:
//   void on_order_update(const OrderUpdate&, const Order&);       // reports from the exchange
//   void on_request_dropped(OrderSlot, ReqKind, DropReason);      // never reached the exchange
//   void on_session(bool up, CloseReason why);                    // why is meaningful when !up
//   void on_kill_switch(KillReason);
//   void on_mass_cancel(const MassCancelReport&);

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "obl/gw/audit.hpp"
#include "obl/gw/instrument.hpp"
#include "obl/gw/metrics.hpp"
#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/order_template.hpp"
#include "obl/gw/ocgc/report_adapter.hpp"
#include "obl/gw/ocgc/session.hpp"
#include "obl/gw/order_state.hpp"
#include "obl/gw/risk.hpp"
#include "obl/gw/throttle.hpp"

namespace obl::gw::ocgc {

enum class SendStatus : std::uint8_t {
  Sent,
  Queued,              // waiting for throttle room; will be sent or reported dropped
  CancelledLocally,    // cancel of an order still queued: it never went out
  RejectedRisk,
  RejectedSessionDown,
  RejectedThrottleFull,
  RejectedNoIds,       // out of order slots / request IDs for the day
  RejectedState,       // order not live, or a request already in flight
};

enum class DropReason : std::uint8_t {
  SessionDown,   // queued when the session went down
  Expired,       // queued longer than ThrottleConfig::max_new_age_ns
  KillSwitch,
  NotSent,       // gap-filled instead of replayed after a reconnect (ReplayPolicy::GapFillBusiness)
  BusinessReject,  // OCG-C Business Message Reject or session Reject (e.g. throttle exceeded)
};

struct SendResult {
  SendStatus status;
  OrderSlot slot = kNoOrder;
  RiskReject risk = RiskReject::None;
};

struct GatewayConfig {
  SessionConfig session;
  std::string broker_id;   // Submitting Broker ID
  std::string bcan;        // Submitting BCAN Field
  std::uint32_t trade_date = 0;  // YYYYMMDD
  std::uint8_t order_capacity = 0;
  ThrottleConfig throttle;
  std::uint8_t route = 0;  // this gateway's index when several share an order table
};

inline WireSide wire_side(OrdSide s) {
  switch (s) {
    case OrdSide::Buy: return WireSide::Buy;
    case OrdSide::Sell: return WireSide::Sell;
    case OrdSide::SellShort: return WireSide::SellShort;
  }
  return WireSide::Buy;
}
inline Tif wire_tif(OrdTif t) {
  switch (t) {
    case OrdTif::Day: return Tif::Day;
    case OrdTif::IOC: return Tif::IOC;
    case OrdTif::FOK: return Tif::FOK;
    case OrdTif::AtCrossing: return Tif::AtCrossing;
  }
  return Tif::Day;
}
inline std::uint64_t us_of_day(std::uint64_t now_ns) { return now_ns / 1000 % 86'400'000'000ull; }

template <class Transport, class Listener>
class OcgcGateway {
 public:
  OcgcGateway(GatewayConfig cfg, Transport& tx, Listener& app, InstrumentTable& ins, OrderTable& orders,
              RiskEngine& risk)
      : cfg_(std::move(cfg)),
        app_(app),
        ins_(ins),
        orders_(orders),
        risk_(risk),
        session_(cfg_.session, tx, *this),
        throttle_(cfg_.throttle) {}

  OcgcGateway(const OcgcGateway&) = delete;
  OcgcGateway& operator=(const OcgcGateway&) = delete;

  using SessionT = Session<Transport, OcgcGateway>;
  SessionT& session() { return session_; }
  const GatewayMetrics& metrics() const { return metrics_; }
  // Optional: audit log of every message in and out; control channel run in poll().
  void set_audit(AuditLog* a) { audit_ = a; }
  void set_control(ControlChannel* c) { control_ = c; }
  Throttle& throttle_mut() { return throttle_; }
  const Throttle& throttle() const { return throttle_; }
  std::uint8_t route() const { return cfg_.route; }
  bool active() const { return session_.active(); }

  // --- connection ---------------------------------------------------------------------------

  void on_connected(std::uint64_t now) {
    now_ = now;
    session_.on_connected(now);
  }
  void on_disconnected(std::uint64_t now) {
    now_ = now;
    const bool was_up = session_.state() != SessionState::Disconnected;
    session_.on_disconnected();
    if (was_up) session_down(CloseReason::ConnectionLost);
  }
  void on_bytes(const std::uint8_t* p, std::size_t n, std::uint64_t now) {
    now_ = now;
    session_.on_bytes(p, n, now);
  }
  void logout(std::uint64_t now) {
    now_ = now;
    session_.logout(now, "end of day");
  }

  // Timers: heartbeats, throttle queue, expiry, kill switch. Call often (every loop iteration
  // when busy polling; at least every few milliseconds otherwise).
  void poll(std::uint64_t now) {
    now_ = now;
    if (control_) [[unlikely]]
      control_->run_pending();
    session_.on_timer(now);
    if (risk_.kill_switch().engaged() && !kill_handled_) [[unlikely]]
      handle_kill(now);
    throttle_.expire(now, [&](const Queued& q) { drop(q, DropReason::Expired); });
    if (session_.active()) drain(now);
  }

  // --- strategy requests ----------------------------------------------------------------------

  SendResult new_order(SymbolIdx sym, OrdSide side, std::int64_t price, std::int64_t qty, std::uint64_t now,
                       OrdTif tif = OrdTif::Day, std::uint64_t tag = 0) {
    now_ = now;
    if (!session_.active()) [[unlikely]]
      return {SendStatus::RejectedSessionDown};
    if (const RiskReject r = risk_.check_new(sym, side, price, qty, now, tif); r != RiskReject::None) [[unlikely]] {
      metrics_.risk_rejects[static_cast<std::size_t>(r)].inc();
      return {SendStatus::RejectedRisk, kNoOrder, r};
    }
    const OrderSlot s = orders_.new_order(sym, side, price, qty, tif, tag);
    if (s == kNoOrder) [[unlikely]]
      return {SendStatus::RejectedNoIds};
    orders_.set_route(s, cfg_.route);
    risk_.on_new_sent(sym, side, price, qty, now);
    if (throttle_.admit(ReqKind::New, now)) [[likely]] {
      send_new(s, now);
      return {SendStatus::Sent, s};
    }
    return queue(ReqKind::New, s, orders_.order(s).req_id, now);
  }

  SendStatus cancel(OrderSlot s, std::uint64_t now) {
    now_ = now;
    const Order& o = orders_.order(s);
    if (o.state == OrdState::PendingNew && orders_.is_unsent(s)) {  // still in our queue
      throttle_.remove(ReqKind::New, o.req_id);
      orders_.unsend(o.req_id);
      return SendStatus::CancelledLocally;
    }
    if ((o.pending & kPendingAmend) && orders_.is_unsent(s)) {  // drop the queued amend first
      throttle_.remove(ReqKind::Amend, o.pending_req);
      orders_.unsend(o.pending_req);
    }
    if (!session_.active()) return SendStatus::RejectedSessionDown;
    const std::uint32_t req = orders_.cancel(s);
    if (!req) return SendStatus::RejectedState;
    if (throttle_.admit(ReqKind::Cancel, now)) {
      send_cancel(s, now);
      return SendStatus::Sent;
    }
    return queue(ReqKind::Cancel, s, req, now).status;
  }

  SendResult amend(OrderSlot s, std::int64_t new_price, std::int64_t new_qty, std::uint64_t now) {
    now_ = now;
    if (!session_.active()) return {SendStatus::RejectedSessionDown, s};
    if (const RiskReject r = risk_.check_amend(s, new_price, new_qty, now); r != RiskReject::None) {
      metrics_.risk_rejects[static_cast<std::size_t>(r)].inc();
      return {SendStatus::RejectedRisk, s, r};
    }
    if (orders_.retarget_amend(s, new_price, new_qty)) return {SendStatus::Queued, s};  // queued one updated
    const std::uint32_t req = orders_.amend(s, new_price, new_qty);
    if (!req) return {SendStatus::RejectedState, s};
    if (throttle_.admit(ReqKind::Amend, now)) {
      send_amend(s, now);
      return {SendStatus::Sent, s};
    }
    return queue(ReqKind::Amend, s, req, now);
  }

  // Cancel all of this session's orders at the exchange (sym = kNoSymbol), or one security's.
  SendStatus mass_cancel(std::uint64_t now, SymbolIdx sym = kNoSymbol) {
    now_ = now;
    if (!session_.active()) return SendStatus::RejectedSessionDown;
    const std::uint32_t req = orders_.mass_cancel_id();
    if (!req) return SendStatus::RejectedNoIds;
    const std::uint8_t type = sym == kNoSymbol ? mass_cancel::kAllOrders : mass_cancel::kForSecurity;
    const std::size_t n = encode_mass_cancel(buf_, 0, ctx(), req, type,
                                             sym == kNoSymbol ? std::string_view{} : ins_[sym].security_id, 0,
                                             us_of_day(now));
    // risk-reducing: not held back by the throttle (OCG-C may still reject it if over the limit)
    throttle_.admit(ReqKind::MassCancel, now);
    session_.send_business(buf_, n, now);
    metrics_.mass_cancels_sent.inc();
    return SendStatus::Sent;
  }

  void set_reference_price(SymbolIdx sym, std::int64_t px) { risk_.set_reference_price(sym, px); }

  // Build every New Order template of a symbol now, so the first order of each kind does not
  // pay for building one.
  void prepare_templates(SymbolIdx sym) {
    for (OrdSide sd : {OrdSide::Buy, OrdSide::Sell, OrdSide::SellShort})
      for (OrdTif t : {OrdTif::Day, OrdTif::IOC, OrdTif::FOK, OrdTif::AtCrossing}) tmpl(sym, sd, t);
  }

  // After a person resets the kill switch.
  void kill_switch_reset() {
    risk_.kill_switch().reset();
    kill_handled_ = false;
  }

  // --- Session handler interface (called by Session) -------------------------------------------

  void on_inbound_frame(const std::uint8_t* m, std::size_t n) {
    metrics_.msgs_in.inc();
    if (audit_) audit_->log(AuditDir::In, cfg_.route, now_, m, n);
  }
  void on_outbound_frame(const std::uint8_t* m, std::size_t n) {
    metrics_.msgs_out.inc();
    if (audit_) audit_->log(AuditDir::Out, cfg_.route, now_, m, n);
  }

  void on_session_active(const SessionFields&) {
    metrics_.session_ups.inc();
    app_.on_session(true, CloseReason::LogoutComplete);
    // engaged while we were disconnected: the orders may still be live at the exchange
    if (risk_.kill_switch().engaged() && kill_handled_) mass_cancel(now_);
    drain(now_);
  }

  void on_session_closed(CloseReason r) { session_down(r); }

  void on_not_sent(std::uint32_t, const std::uint8_t* msg, std::size_t len) {
    const Header h = read_header(msg);
    OrderRequest req;
    if (!decode_request(msg, len, h.type, req)) return;
    const OrderSlot s = orders_.slot_of(req.cl_ord_id);
    const ReqKind k = orders_.kind_of(req.cl_ord_id);
    orders_.unsend(req.cl_ord_id);
    count_drop(DropReason::NotSent);
    if (s != kNoOrder) app_.on_request_dropped(s, k, DropReason::NotSent);
  }

  void on_business(const Header& h, const std::uint8_t* msg, std::size_t len) {
    switch (h.type) {
      case MsgType::ExecutionReport: {
        ExecReport er;
        if (!decode_exec_report(msg, len, er)) return;
        const OrderUpdate u = orders_.apply(to_report(er));
        if (u.slot == kNoOrder) return;
        switch (u.event) {
          case OrderEvent::Filled:
            metrics_.fills.inc();
            risk_.on_position_change(orders_.order(u.slot).symbol);
            break;
          case OrderEvent::FillBusted: risk_.on_position_change(orders_.order(u.slot).symbol); break;
          case OrderEvent::Rejected:
            metrics_.rejects_exchange.inc();
            risk_.on_exchange_reject(now_);
            break;
          case OrderEvent::Acked: metrics_.acks.inc(); break;
          case OrderEvent::Cancelled: metrics_.cancels.inc(); break;
          default: break;
        }
        if (u.event != OrderEvent::Duplicate) app_.on_order_update(u, orders_.order(u.slot));
        return;
      }
      case MsgType::BusinessMessageReject:
      case MsgType::Reject: {
        RejectInfo ri;
        if (!decode_reject(msg, len, h.type, ri)) return;
        const std::uint32_t id = parse_cl_ord_id(ri.ref_id);
        const OrderSlot s = orders_.slot_of(id);
        if (s == kNoOrder) return;
        const ReqKind k = orders_.kind_of(id);
        metrics_.business_rejects.inc();
        risk_.on_exchange_reject(now_);
        orders_.unsend(id);  // the request was not accepted: as if never sent
        app_.on_request_dropped(s, k, DropReason::BusinessReject);
        return;
      }
      case MsgType::OrderMassCancelReport: {
        MassCancelReport m;
        if (!decode_mass_cancel_report(msg, len, m)) return;
        if (m.response == 0) cancel_all_individually(now_);  // mass cancel refused: one by one
        app_.on_mass_cancel(m);
        return;
      }
      default:
        return;
    }
  }

 private:
  OrderContext ctx() const { return {cfg_.session.comp_id, cfg_.broker_id, cfg_.trade_date}; }

  NewOrderTemplate& tmpl(SymbolIdx sym, OrdSide side, OrdTif tif) {
    const std::size_t i = (static_cast<std::size_t>(sym) * 3 + static_cast<std::size_t>(side)) * 4 +
                          static_cast<std::size_t>(tif);
    if (i >= templates_.size()) templates_.resize(i + 1);
    if (!templates_[i]) [[unlikely]] {
      NewOrderStatic st;
      st.comp_id = cfg_.session.comp_id;
      st.submitting_broker_id = cfg_.broker_id;
      st.security_id = ins_[sym].security_id;
      st.bcan = cfg_.bcan;
      st.side = wire_side(side);
      st.trade_date = cfg_.trade_date;
      st.tif = wire_tif(tif);
      st.order_capacity = cfg_.order_capacity;
      templates_[i] = std::make_unique<NewOrderTemplate>(st);
    }
    return *templates_[i];
  }

  void send_new(OrderSlot s, std::uint64_t now) {
    const Order& o = orders_.order(s);
    const NewOrderVar v{0, o.req_id, us_of_day(now), o.price, o.qty};
    session_.send_new_order(tmpl(o.symbol, o.side, o.tif), v, now);
    orders_.mark_sent(s);
    metrics_.orders_sent.inc();
  }
  void send_cancel(OrderSlot s, std::uint64_t now) {
    const Order& o = orders_.order(s);
    const std::size_t n = encode_cancel(buf_, 0, ctx(), ins_[o.symbol].security_id, wire_side(o.side),
                                        o.pending_req, o.req_id, us_of_day(now));
    session_.send_business(buf_, n, now);
    orders_.mark_sent(s);
    metrics_.cancels_sent.inc();
  }
  void send_amend(OrderSlot s, std::uint64_t now) {
    const Order& o = orders_.order(s);
    const std::size_t n = encode_amend(buf_, 0, ctx(), ins_[o.symbol].security_id, wire_side(o.side), o.pending_req,
                                       o.req_id, o.amend_price, o.amend_qty, wire_tif(o.tif), us_of_day(now));
    session_.send_business(buf_, n, now);
    orders_.mark_sent(s);
    metrics_.amends_sent.inc();
  }

  SendResult queue(ReqKind k, OrderSlot s, std::uint32_t req, std::uint64_t now) {
    if (!throttle_.enqueue(k, s, req, now)) {
      orders_.unsend(req);
      return {SendStatus::RejectedThrottleFull, s};
    }
    orders_.mark_unsent(s);
    metrics_.queued.inc();
    return {SendStatus::Queued, s};
  }

  void drain(std::uint64_t now) {
    throttle_.drain(now, [&](const Queued& q) {
      if (!session_.active()) return false;
      switch (q.kind) {
        case ReqKind::New: send_new(q.slot, now); break;
        case ReqKind::Cancel: send_cancel(q.slot, now); break;
        case ReqKind::Amend: send_amend(q.slot, now); break;
        default: break;
      }
      return true;
    });
  }

  void drop(const Queued& q, DropReason why) {
    count_drop(why);
    orders_.unsend(q.req_id);
    app_.on_request_dropped(q.slot, q.kind, why);
  }

  void count_drop(DropReason why) {
    switch (why) {
      case DropReason::SessionDown: metrics_.dropped_session_down.inc(); break;
      case DropReason::Expired: metrics_.dropped_expired.inc(); break;
      case DropReason::KillSwitch: metrics_.dropped_kill.inc(); break;
      case DropReason::NotSent: metrics_.dropped_not_sent.inc(); break;
      case DropReason::BusinessReject: break;
    }
  }

  void session_down(CloseReason r) {
    metrics_.session_downs.inc();
    // Nothing queued may go out late after a reconnect: drop it all and tell the strategy.
    for (ReqKind k : {ReqKind::Cancel, ReqKind::Amend, ReqKind::New})
      throttle_.clear(k, [&](const Queued& q) { drop(q, DropReason::SessionDown); });
    app_.on_session(false, r);
  }

  void handle_kill(std::uint64_t now) {
    kill_handled_ = true;
    throttle_.clear(ReqKind::New, [&](const Queued& q) { drop(q, DropReason::KillSwitch); });
    throttle_.clear(ReqKind::Amend, [&](const Queued& q) { drop(q, DropReason::KillSwitch); });
    app_.on_kill_switch(risk_.kill_switch().reason());
    if (session_.active()) mass_cancel(now);
  }

  void cancel_all_individually(std::uint64_t now) {
    for (SymbolIdx sym = 0; sym < ins_.size(); ++sym) {
      orders_.for_each_live(sym, [&](OrderSlot s, const Order& o) {
        if (orders_.route(s) == cfg_.route && !(o.pending & kPendingCancel)) cancel(s, now);
      });
    }
  }

  GatewayConfig cfg_;
  Listener& app_;
  InstrumentTable& ins_;
  OrderTable& orders_;
  RiskEngine& risk_;
  SessionT session_;
  Throttle throttle_;
  std::vector<std::unique_ptr<NewOrderTemplate>> templates_;
  alignas(64) std::uint8_t buf_[512];
  std::uint64_t now_ = 0;
  bool kill_handled_ = false;
  GatewayMetrics metrics_;
  AuditLog* audit_ = nullptr;
  ControlChannel* control_ = nullptr;
};


// Spreads new orders over several sessions (each with its own throttle) that share one order
// table: the active session with the most throttle room gets the next order; cancels and amends
// follow the order's route.
template <class Gateway>
class SessionRouter {
 public:
  void add(Gateway& g) { gws_.push_back(&g); }

  // nullptr if no session is up.
  Gateway* pick(std::uint64_t now) const {
    Gateway* best = nullptr;
    std::uint32_t best_room = 0;
    for (Gateway* g : gws_) {
      if (!g->active()) continue;
      const Throttle& t = g->throttle();
      const std::uint32_t room =
          t.limit() == 0 ? 0xFFFFFFFFu - t.queued_total() : t.limit() - std::min(t.limit(), t.window_used(now) + t.queued_total());
      if (!best || room > best_room) {
        best = g;
        best_room = room;
      }
    }
    return best;
  }
  Gateway& for_order(const OrderTable& orders, OrderSlot s) const { return *gws_[orders.route(s)]; }
  std::size_t size() const { return gws_.size(); }

 private:
  std::vector<Gateway*> gws_;
};

}  // namespace obl::gw::ocgc
