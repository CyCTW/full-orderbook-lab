#pragma once

// OCG-C session-level message encoders (§7.5) and business message decoders (Execution Report,
// Reject, Business Message Reject).

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "obl/gw/ocgc/protocol.hpp"

namespace obl::gw::ocgc {

// ---------------------------------------------------------------------------------------------
// Session-level encoders. All return the message length; `buf` needs room for the message
// (Logon with a password: 54 + 450 + 4 + 4 = 512 bytes).

inline std::size_t encode_logon(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                std::string_view encrypted_password, std::uint32_t next_expected) {
  Writer w(buf, MsgType::Logon, seq, comp_id);
  w.alnum(logon::Password, encrypted_password, field_size::kPassword);
  w.u32(logon::NextExpectedSeq, next_expected);
  return w.finish();
}

// Logon reply as sent by OCG-C (used by tests and the exchange simulator).
inline std::size_t encode_logon_reply(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                      std::uint32_t next_expected, SessionStatus status = SessionStatus::Active) {
  Writer w(buf, MsgType::Logon, seq, comp_id);
  w.u32(logon::NextExpectedSeq, next_expected);
  w.u8(logon::SessionStatus, static_cast<std::uint8_t>(status));
  return w.finish();
}

inline std::size_t encode_logout(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                 std::string_view text = {}) {
  Writer w(buf, MsgType::Logout, seq, comp_id);
  if (!text.empty()) w.var_alnum(logout::LogoutText, text.substr(0, field_size::kReasonMax - 1));
  return w.finish();
}

inline std::size_t encode_heartbeat(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                    int ref_test_req_id = -1) {
  Writer w(buf, MsgType::Heartbeat, seq, comp_id);
  if (ref_test_req_id >= 0) w.u16(heartbeat::RefTestReqId, static_cast<std::uint16_t>(ref_test_req_id));
  return w.finish();
}

inline std::size_t encode_test_request(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                       std::uint16_t test_req_id) {
  Writer w(buf, MsgType::TestRequest, seq, comp_id);
  w.u16(test_request::TestReqId, test_req_id);
  return w.finish();
}

inline std::size_t encode_resend_request(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                         std::uint32_t start, std::uint32_t end) {
  Writer w(buf, MsgType::ResendRequest, seq, comp_id);
  w.u32(resend_request::StartSeq, start);
  w.u32(resend_request::EndSeq, end);
  return w.finish();
}

inline std::size_t encode_sequence_reset(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                         std::uint32_t new_seq, bool gap_fill, bool poss_dup) {
  Writer w(buf, MsgType::SequenceReset, seq, comp_id, poss_dup);
  w.u8(sequence_reset::GapFill, static_cast<std::uint8_t>(gap_fill ? sequence_reset::kGapFill : sequence_reset::kReset));
  w.u32(sequence_reset::NewSeqNo, new_seq);
  return w.finish();
}

// ---------------------------------------------------------------------------------------------
// Decoded session-level fields (whatever is present; zero / empty otherwise)

struct SessionFields {
  std::uint32_t next_expected = 0;   // Logon
  int session_status = -1;           // Logon, Logout
  int test_req_id = -1;              // Test Request; Heartbeat (Reference Test Request ID)
  std::uint32_t start_seq = 0;       // Resend Request
  std::uint32_t end_seq = 0;
  char gap_fill = sequence_reset::kReset;  // Sequence Reset
  std::uint32_t new_seq = 0;
  std::string_view text;             // Logon Text, Logout Text
};

inline bool decode_session(const std::uint8_t* msg, std::size_t len, MsgType type, SessionFields& out) {
  const FieldTable* defs = fields_for(type);
  if (!defs) return false;
  return for_each_field_unchecked(msg, len, *defs, [&](const FieldRef& f) {
    switch (type) {
      case MsgType::Logon:
        if (f.bit == logon::NextExpectedSeq) out.next_expected = as_u32(f);
        else if (f.bit == logon::SessionStatus) out.session_status = as_u8(f);
        else if (f.bit == logon::Text) out.text = as_str(f);
        break;
      case MsgType::Logout:
        if (f.bit == logout::LogoutText) out.text = as_str(f);
        else if (f.bit == logout::SessionStatus) out.session_status = as_u8(f);
        break;
      case MsgType::Heartbeat:
      case MsgType::TestRequest:
        out.test_req_id = as_u16(f);  // bit 0 in both
        break;
      case MsgType::ResendRequest:
        if (f.bit == resend_request::StartSeq) out.start_seq = as_u32(f);
        else out.end_seq = as_u32(f);
        break;
      case MsgType::SequenceReset:
        if (f.bit == sequence_reset::GapFill) out.gap_fill = static_cast<char>(as_u8(f));
        else out.new_seq = as_u32(f);
        break;
      default:
        break;
    }
  });
}

// ---------------------------------------------------------------------------------------------
// Execution Report

// A text field located but not yet measured: the NUL is searched only when the value is read,
// so decoding a report does not pay for strings the caller never looks at.
struct LazyStr {
  const char* p = nullptr;
  std::uint16_t cap = 0;

  LazyStr() = default;
  LazyStr(std::string_view s) : p(s.data()), cap(static_cast<std::uint16_t>(s.size())) {}
  LazyStr(const char* s) : LazyStr(std::string_view(s)) {}
  static LazyStr raw(const std::uint8_t* data, std::size_t size) {
    LazyStr l;
    l.p = reinterpret_cast<const char*>(data);
    l.cap = static_cast<std::uint16_t>(size);
    return l;
  }
  std::string_view view() const {
    if (!p) return {};
    const void* z = std::memchr(p, 0, cap);
    return {p, z ? static_cast<std::size_t>(static_cast<const char*>(z) - p) : cap};
  }
  operator std::string_view() const { return view(); }
  friend bool operator==(const LazyStr& a, std::string_view b) { return a.view() == b; }
  friend bool operator==(const LazyStr& a, const char* b) { return a.view() == std::string_view(b); }
  friend bool operator==(const LazyStr& a, const LazyStr& b) { return a.view() == b.view(); }
};

struct ExecReport {
  std::uint64_t present = 0;  // bit i set = field with bit position i present (all ER bits < 64)
  LazyStr cl_ord_id, orig_cl_ord_id, order_id, exec_id, ref_exec_id, security_id, submitting_broker_id;
  LazyStr transact_time, reason, text, trade_match_id, counterparty_broker_id;
  ExecType exec_type{};
  OrdStatus ord_status{};
  std::uint8_t side = 0, ord_type = 0, tif = 0, lot_type = 0, match_type = 0, aggressor = 0;
  std::int64_t price = 0, order_qty = 0, cum_qty = 0, leaves_qty = 0, exec_qty = 0, exec_price = 0;
  std::uint16_t order_reject_code = 0, cancel_reject_code = 0, amend_reject_code = 0, restatement_reason = 0;

  bool has(exec_report::Bit b) const { return present >> b & 1; }
};

// Decodes a validated Execution Report. Fields not listed in ExecReport are skipped.
inline bool decode_exec_report(const std::uint8_t* msg, std::size_t len, ExecReport& er) {
  namespace b = exec_report;
  er.present = 0;
  return for_each_field_unchecked(msg, len, b::kFields, [&](const FieldRef& f) {
    er.present |= std::uint64_t{1} << f.bit;
    switch (f.bit) {
      case b::ClOrdId: er.cl_ord_id = LazyStr::raw(f.data, f.size); break;
      case b::SubmittingBrokerId: er.submitting_broker_id = LazyStr::raw(f.data, f.size); break;
      case b::SecurityId: er.security_id = LazyStr::raw(f.data, f.size); break;
      case b::TransactTime: er.transact_time = LazyStr::raw(f.data, f.size); break;
      case b::Side: er.side = as_u8(f); break;
      case b::OrigClOrdId: er.orig_cl_ord_id = LazyStr::raw(f.data, f.size); break;
      case b::OrderId: er.order_id = LazyStr::raw(f.data, f.size); break;
      case b::OrdType: er.ord_type = as_u8(f); break;
      case b::Price: er.price = as_decimal(f); break;
      case b::OrderQty: er.order_qty = as_decimal(f); break;
      case b::TimeInForce: er.tif = as_u8(f); break;
      case b::Text: er.text = LazyStr::raw(f.data, f.size); break;
      case b::Reason: er.reason = LazyStr::raw(f.data, f.size); break;
      case b::ExecId: er.exec_id = LazyStr::raw(f.data, f.size); break;
      case b::OrdStatus: er.ord_status = static_cast<OrdStatus>(as_u8(f)); break;
      case b::ExecType: er.exec_type = static_cast<ExecType>(as_u8(f)); break;
      case b::CumQty: er.cum_qty = as_decimal(f); break;
      case b::LeavesQty: er.leaves_qty = as_decimal(f); break;
      case b::OrderRejectCode: er.order_reject_code = as_u16(f); break;
      case b::LotType: er.lot_type = as_u8(f); break;
      case b::ExecRestatementReason: er.restatement_reason = as_u16(f); break;
      case b::CancelRejectCode: er.cancel_reject_code = as_u16(f); break;
      case b::MatchType: er.match_type = as_u8(f); break;
      case b::CounterpartyBrokerId: er.counterparty_broker_id = LazyStr::raw(f.data, f.size); break;
      case b::ExecQty: er.exec_qty = as_decimal(f); break;
      case b::ExecPrice: er.exec_price = as_decimal(f); break;
      case b::RefExecId: er.ref_exec_id = LazyStr::raw(f.data, f.size); break;
      case b::AmendRejectCode: er.amend_reject_code = as_u16(f); break;
      case b::TradeMatchId: er.trade_match_id = LazyStr::raw(f.data, f.size); break;
      case b::AggressorIndicator: er.aggressor = as_u8(f); break;
      default: break;
    }
  });
}

// Encodes the fields flagged in er.present (only the fields ExecReport models; Security ID Source
// and Security Exchange are written as 8 / "XHKG"). Used by tests and the exchange simulator.
inline std::size_t encode_exec_report(std::uint8_t* buf, std::uint32_t seq, std::string_view comp_id,
                                      const ExecReport& er, bool poss_dup = false, bool poss_resend = false) {
  namespace b = exec_report;
  namespace fs = field_size;
  Writer w(buf, MsgType::ExecutionReport, seq, comp_id, poss_dup, poss_resend);
  for (unsigned bit = 0; bit < 64; ++bit) {
    if (!(er.present >> bit & 1)) continue;
    switch (bit) {
      case b::ClOrdId: w.alnum(bit, er.cl_ord_id.view(), fs::kClOrdId); break;
      case b::SubmittingBrokerId: w.alnum(bit, er.submitting_broker_id.view(), fs::kBrokerId); break;
      case b::SecurityId: w.alnum(bit, er.security_id.view(), fs::kSecurityId); break;
      case b::SecurityIdSource: w.u8(bit, kSecurityIdSourceExchangeSymbol); break;
      case b::SecurityExchange: w.alnum(bit, "XHKG", fs::kSecurityExchange); break;
      case b::TransactTime: w.alnum(bit, er.transact_time.view(), fs::kTransactTime); break;
      case b::Side: w.u8(bit, er.side); break;
      case b::OrigClOrdId: w.alnum(bit, er.orig_cl_ord_id.view(), fs::kClOrdId); break;
      case b::OrderId: w.alnum(bit, er.order_id.view(), fs::kOrderId); break;
      case b::OrdType: w.u8(bit, er.ord_type); break;
      case b::Price: w.decimal(bit, er.price); break;
      case b::OrderQty: w.decimal(bit, er.order_qty); break;
      case b::TimeInForce: w.u8(bit, er.tif); break;
      case b::Text: w.var_alnum(bit, er.text.view()); break;
      case b::Reason: w.var_alnum(bit, er.reason.view()); break;
      case b::ExecId: w.alnum(bit, er.exec_id.view(), fs::kExecId); break;
      case b::OrdStatus: w.u8(bit, static_cast<std::uint8_t>(er.ord_status)); break;
      case b::ExecType: w.u8(bit, static_cast<std::uint8_t>(er.exec_type)); break;
      case b::CumQty: w.decimal(bit, er.cum_qty); break;
      case b::LeavesQty: w.decimal(bit, er.leaves_qty); break;
      case b::OrderRejectCode: w.u16(bit, er.order_reject_code); break;
      case b::LotType: w.u8(bit, er.lot_type); break;
      case b::ExecRestatementReason: w.u16(bit, er.restatement_reason); break;
      case b::CancelRejectCode: w.u16(bit, er.cancel_reject_code); break;
      case b::MatchType: w.u8(bit, er.match_type); break;
      case b::CounterpartyBrokerId: w.alnum(bit, er.counterparty_broker_id.view(), fs::kBrokerId); break;
      case b::ExecQty: w.decimal(bit, er.exec_qty); break;
      case b::ExecPrice: w.decimal(bit, er.exec_price); break;
      case b::RefExecId: w.alnum(bit, er.ref_exec_id.view(), fs::kExecId); break;
      case b::AmendRejectCode: w.u16(bit, er.amend_reject_code); break;
      case b::TradeMatchId: w.alnum(bit, er.trade_match_id.view(), fs::kTradeMatchId); break;
      case b::AggressorIndicator: w.u8(bit, er.aggressor); break;
      default: assert(false && "field not modelled by ExecReport"); break;
    }
  }
  return w.finish();
}

// Client Order ID text -> number (§6.6.3.1: digits only, 1..99,999,999). Returns 0 if invalid.
inline std::uint32_t parse_cl_ord_id(std::string_view s) {
  if (s.empty() || s.size() > 8 || s[0] == '0') return 0;
  std::uint32_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return 0;
    v = v * 10 + static_cast<std::uint32_t>(c - '0');
  }
  return v;
}

// ---------------------------------------------------------------------------------------------
// Reject (3) / Business Message Reject (9)

struct RejectInfo {
  std::uint16_t code = 0;
  std::uint8_t ref_msg_type = 0;
  std::uint32_t ref_seq = 0;
  std::string_view reason, ref_field, ref_id;  // ref_id: Client Order ID / Business Reject Reference ID
};

inline bool decode_reject(const std::uint8_t* msg, std::size_t len, MsgType type, RejectInfo& out) {
  if (type != MsgType::Reject && type != MsgType::BusinessMessageReject) return false;
  // Both messages share the same bit layout (§7.5.6, §7.9.1).
  static_assert(int{reject::RefSeqNum} == int{business_reject::RefSeqNum} &&
                int{reject::ClOrdId} == int{business_reject::BusinessRejectRefId});
  return for_each_field_unchecked(msg, len, *fields_for(type), [&](const FieldRef& f) {
    switch (f.bit) {
      case reject::MessageRejectCode: out.code = as_u16(f); break;
      case reject::Reason: out.reason = as_str(f); break;
      case reject::RefMsgType: out.ref_msg_type = as_u8(f); break;
      case reject::RefFieldName: out.ref_field = as_str(f); break;
      case reject::RefSeqNum: out.ref_seq = as_u32(f); break;
      case reject::ClOrdId: out.ref_id = as_str(f); break;
      default: break;
    }
  });
}

}  // namespace obl::gw::ocgc
