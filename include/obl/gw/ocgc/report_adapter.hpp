#pragma once

// OCG-C Execution Report -> protocol-neutral gw::Report for the order table.

#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/order_state.hpp"

namespace obl::gw::ocgc {

inline Report to_report(const ExecReport& er) {
  namespace b = exec_report;
  Report r;
  r.req_id = parse_cl_ord_id(er.cl_ord_id);
  if (er.has(b::OrigClOrdId)) r.orig_req_id = parse_cl_ord_id(er.orig_cl_ord_id);
  switch (er.exec_type) {
    case ExecType::New: r.kind = ReportKind::Ack; break;
    case ExecType::Reject: r.kind = ReportKind::Reject; r.reject_code = er.order_reject_code; break;
    case ExecType::Trade: r.kind = ReportKind::Fill; break;
    case ExecType::Cancel: r.kind = ReportKind::Cancelled; break;
    case ExecType::Expire: r.kind = ReportKind::Expired; break;
    case ExecType::Amend: r.kind = ReportKind::Amended; break;
    case ExecType::CancelReject: r.kind = ReportKind::CancelReject; r.reject_code = er.cancel_reject_code; break;
    case ExecType::AmendReject: r.kind = ReportKind::AmendReject; r.reject_code = er.amend_reject_code; break;
    case ExecType::TradeCancel: r.kind = ReportKind::TradeCancel; break;
    default: r.kind = ReportKind::Other; break;
  }
  if (er.has(b::Price)) r.price = er.price;
  if (er.has(b::OrderQty)) r.order_qty = er.order_qty;
  if (er.has(b::CumQty)) r.cum_qty = er.cum_qty;
  if (er.has(b::LeavesQty)) r.leaves_qty = er.leaves_qty;
  r.exec_qty = er.exec_qty;
  r.exec_price = er.exec_price;
  r.exchange_order_id = er.order_id.view();
  return r;
}

}  // namespace obl::gw::ocgc
