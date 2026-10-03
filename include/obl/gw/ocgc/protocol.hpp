#pragma once

// HKEX OCG-C Binary Trading Protocol (Orion Central Gateway - Securities Market), v3.2.
// Source: "Interface Specifications HKEX Orion Central Gateway - Securities Market, Binary Trading
// Protocol", v3.2 (19 July 2023; adds Self-Match Prevention). Sections cited as §x.y.
//
// Message = header + body + trailer, all little-endian (§6.1, §6.2, §7.2, §7.3):
//
//   off  size  field
//     0     1  Start of Message   UInt8, always STX (0x02)
//     1     2  Length             UInt16, whole message incl. header and trailer
//     3     1  Message Type       UInt8
//     4     4  Sequence Number    UInt32
//     8     1  PossDup            UInt8
//     9     1  PossResend         UInt8
//    10    12  Comp ID            Alphanumeric(12)
//    22    32  Body Fields Presence Map   256 bits, bit 0 = MSB of byte 0
//    54     -  body: the present fields, in bit order, back to back
//   n-4     4  Checksum           UInt32, CRC32C of bytes [0, n-4)  (§4.8)
//
// Alphanumeric(n) fields are NUL terminated and the NUL counts in n; bytes after the NUL are
// ignored (§6.1). Decimal is Int64 with 8 implied decimal places (price and quantity both).

#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "obl/common.hpp"
#include "obl/gw/ocgc/crc32c.hpp"

namespace obl::gw::ocgc {

inline constexpr std::uint8_t kStx = 0x02;

namespace hdr {
inline constexpr std::size_t kStart = 0;
inline constexpr std::size_t kLength = 1;
inline constexpr std::size_t kMsgType = 3;
inline constexpr std::size_t kSeqNum = 4;
inline constexpr std::size_t kPossDup = 8;
inline constexpr std::size_t kPossResend = 9;
inline constexpr std::size_t kCompId = 10;
inline constexpr std::size_t kCompIdSize = 12;
inline constexpr std::size_t kPresenceMap = 22;
inline constexpr std::size_t kPresenceMapSize = 32;
inline constexpr std::size_t kSize = 54;
}  // namespace hdr

inline constexpr std::size_t kTrailerSize = 4;

enum class MsgType : std::uint8_t {
  Heartbeat = 0,
  TestRequest = 1,
  ResendRequest = 2,
  Reject = 3,
  SequenceReset = 4,
  Logon = 5,
  Logout = 6,
  LookupRequest = 7,
  LookupResponse = 8,
  BusinessMessageReject = 9,
  ExecutionReport = 10,
  NewOrder = 11,
  AmendOrder = 12,
  CancelOrder = 13,
  MassCancel = 14,
  OrderMassCancelReport = 15,
};

// Wire values (§7.6.1)
enum class WireSide : std::uint8_t { Buy = 1, Sell = 2, SellShort = 5 };
enum class OrdType : std::uint8_t { Market = 1, Limit = 2 };
enum class Tif : std::uint8_t { Day = 0, IOC = 3, FOK = 4, AtCrossing = 9 };
inline constexpr std::uint8_t kSecurityIdSourceExchangeSymbol = 8;
inline constexpr std::uint16_t kDisclosureNone = 1;  // bit 0 = "nothing to disclose" (§7.6.1)

inline constexpr std::int64_t kDecimalScale = 100'000'000;  // 8 implied decimal places

// ---------------------------------------------------------------------------------------------
// Field dictionaries (§7.6, sizes from §8.2)

enum class FieldKind : std::uint8_t { Unused, Alnum, VarAlnum, Byte, UInt8, UInt16, UInt32, Decimal };

struct FieldDef {
  FieldKind kind = FieldKind::Unused;
  std::uint16_t size = 0;  // fixed size in bytes; VarAlnum: maximum content length
  const char* name = nullptr;
};

namespace field_size {
inline constexpr std::uint8_t kClOrdId = 21;
inline constexpr std::uint8_t kBrokerId = 12;
inline constexpr std::uint8_t kSecurityId = 21;
inline constexpr std::uint8_t kSecurityExchange = 5;
inline constexpr std::uint8_t kBrokerLocationId = 11;
inline constexpr std::uint8_t kTransactTime = 25;  // "YYYYMMDD-HH:MM:SS.ssssss" + NUL, UTC
inline constexpr std::uint8_t kOrderRestrictions = 21;
inline constexpr std::uint8_t kExecInst = 21;
inline constexpr std::uint8_t kBcan = 21;
inline constexpr std::uint8_t kSmpId = 10;
inline constexpr std::uint8_t kOrderId = 21;
inline constexpr std::uint8_t kTextMax = 50;
inline constexpr std::uint8_t kReasonMax = 75;      // Reason, Logout Text
inline constexpr std::uint16_t kPassword = 450;     // RSA-encrypted, PKCS #1 or OAEP padding
inline constexpr std::uint8_t kExecId = 21;
inline constexpr std::uint8_t kTradeMatchId = 25;
inline constexpr std::uint8_t kRefFieldName = 50;
}  // namespace field_size

using FieldTable = std::array<FieldDef, 256>;

// New Board Lot / Odd Lot Order - Single (11), §7.6.1 / §7.6.2
namespace new_order {
enum Bit : std::uint8_t {
  ClOrdId = 0,
  SubmittingBrokerId = 1,
  SecurityId = 2,
  SecurityIdSource = 3,
  SecurityExchange = 4,
  BrokerLocationId = 5,
  TransactTime = 6,
  Side = 7,
  OrdType = 8,
  Price = 9,
  OrderQty = 10,
  TimeInForce = 11,
  PositionEffect = 12,
  OrderRestrictions = 13,
  MaxPriceLevels = 14,
  OrderCapacity = 15,
  Text = 16,
  ExecInst = 17,
  DisclosureInstructions = 18,
  LotType = 19,
  SubmittingBcan = 22,
  SmpId = 23,
};

constexpr FieldTable make_fields() {
  namespace s = field_size;
  FieldTable t{};
  t[ClOrdId] = {FieldKind::Alnum, s::kClOrdId, "ClientOrderID"};
  t[SubmittingBrokerId] = {FieldKind::Alnum, s::kBrokerId, "SubmittingBrokerID"};
  t[SecurityId] = {FieldKind::Alnum, s::kSecurityId, "SecurityID"};
  t[SecurityIdSource] = {FieldKind::UInt8, 1, "SecurityIDSource"};
  t[SecurityExchange] = {FieldKind::Alnum, s::kSecurityExchange, "SecurityExchange"};
  t[BrokerLocationId] = {FieldKind::Alnum, s::kBrokerLocationId, "BrokerLocationID"};
  t[TransactTime] = {FieldKind::Alnum, s::kTransactTime, "TransactionTime"};
  t[Side] = {FieldKind::UInt8, 1, "Side"};
  t[OrdType] = {FieldKind::UInt8, 1, "OrderType"};
  t[Price] = {FieldKind::Decimal, 8, "Price"};
  t[OrderQty] = {FieldKind::Decimal, 8, "OrderQuantity"};
  t[TimeInForce] = {FieldKind::UInt8, 1, "TIF"};
  t[PositionEffect] = {FieldKind::UInt8, 1, "PositionEffect"};
  t[OrderRestrictions] = {FieldKind::Alnum, s::kOrderRestrictions, "OrderRestrictions"};
  t[MaxPriceLevels] = {FieldKind::UInt8, 1, "MaxPriceLevels"};
  t[OrderCapacity] = {FieldKind::UInt8, 1, "OrderCapacity"};
  t[Text] = {FieldKind::VarAlnum, s::kTextMax, "Text"};
  t[ExecInst] = {FieldKind::Alnum, s::kExecInst, "ExecutionInstructions"};
  t[DisclosureInstructions] = {FieldKind::UInt16, 2, "DisclosureInstructions"};
  t[LotType] = {FieldKind::UInt8, 1, "LotType"};
  t[SubmittingBcan] = {FieldKind::Alnum, s::kBcan, "SubmittingBCANField"};
  t[SmpId] = {FieldKind::Alnum, s::kSmpId, "SMPID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace new_order

// Cancel Order (13), §7.6.4
namespace cancel_order {
enum Bit : std::uint8_t {
  ClOrdId = 0,
  SubmittingBrokerId = 1,
  SecurityId = 2,
  SecurityIdSource = 3,
  SecurityExchange = 4,
  BrokerLocationId = 5,
  TransactTime = 6,
  Side = 7,
  OrigClOrdId = 8,
  OrderId = 9,
  Text = 10,
};

constexpr FieldTable make_fields() {
  namespace s = field_size;
  FieldTable t{};
  t[ClOrdId] = {FieldKind::Alnum, s::kClOrdId, "ClientOrderID"};
  t[SubmittingBrokerId] = {FieldKind::Alnum, s::kBrokerId, "SubmittingBrokerID"};
  t[SecurityId] = {FieldKind::Alnum, s::kSecurityId, "SecurityID"};
  t[SecurityIdSource] = {FieldKind::UInt8, 1, "SecurityIDSource"};
  t[SecurityExchange] = {FieldKind::Alnum, s::kSecurityExchange, "SecurityExchange"};
  t[BrokerLocationId] = {FieldKind::Alnum, s::kBrokerLocationId, "BrokerLocationID"};
  t[TransactTime] = {FieldKind::Alnum, s::kTransactTime, "TransactionTime"};
  t[Side] = {FieldKind::UInt8, 1, "Side"};
  t[OrigClOrdId] = {FieldKind::Alnum, s::kClOrdId, "OriginalClientOrderID"};
  t[OrderId] = {FieldKind::Alnum, s::kOrderId, "OrderID"};
  t[Text] = {FieldKind::VarAlnum, s::kTextMax, "Text"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace cancel_order

// Amend Order (12), §7.6.3
namespace amend_order {
enum Bit : std::uint8_t {
  ClOrdId = 0,
  SubmittingBrokerId = 1,
  SecurityId = 2,
  SecurityIdSource = 3,
  SecurityExchange = 4,
  BrokerLocationId = 5,
  TransactTime = 6,
  Side = 7,
  OrigClOrdId = 8,
  OrderId = 9,
  OrdType = 10,
  Price = 11,
  OrderQty = 12,
  TimeInForce = 13,
  PositionEffect = 14,
  OrderRestrictions = 15,
  MaxPriceLevels = 16,
  OrderCapacity = 17,
  Text = 18,
  ExecInst = 19,
  DisclosureInstructions = 20,
};

constexpr FieldTable make_fields() {
  namespace s = field_size;
  FieldTable t{};
  t[ClOrdId] = {FieldKind::Alnum, s::kClOrdId, "ClientOrderID"};
  t[SubmittingBrokerId] = {FieldKind::Alnum, s::kBrokerId, "SubmittingBrokerID"};
  t[SecurityId] = {FieldKind::Alnum, s::kSecurityId, "SecurityID"};
  t[SecurityIdSource] = {FieldKind::UInt8, 1, "SecurityIDSource"};
  t[SecurityExchange] = {FieldKind::Alnum, s::kSecurityExchange, "SecurityExchange"};
  t[BrokerLocationId] = {FieldKind::Alnum, s::kBrokerLocationId, "BrokerLocationID"};
  t[TransactTime] = {FieldKind::Alnum, s::kTransactTime, "TransactionTime"};
  t[Side] = {FieldKind::UInt8, 1, "Side"};
  t[OrigClOrdId] = {FieldKind::Alnum, s::kClOrdId, "OriginalClientOrderID"};
  t[OrderId] = {FieldKind::Alnum, s::kOrderId, "OrderID"};
  t[OrdType] = {FieldKind::UInt8, 1, "OrderType"};
  t[Price] = {FieldKind::Decimal, 8, "Price"};
  t[OrderQty] = {FieldKind::Decimal, 8, "OrderQuantity"};
  t[TimeInForce] = {FieldKind::UInt8, 1, "TIF"};
  t[PositionEffect] = {FieldKind::UInt8, 1, "PositionEffect"};
  t[OrderRestrictions] = {FieldKind::Alnum, s::kOrderRestrictions, "OrderRestrictions"};
  t[MaxPriceLevels] = {FieldKind::UInt8, 1, "MaxPriceLevels"};
  t[OrderCapacity] = {FieldKind::UInt8, 1, "OrderCapacity"};
  t[Text] = {FieldKind::VarAlnum, s::kTextMax, "Text"};
  t[ExecInst] = {FieldKind::Alnum, s::kExecInst, "ExecutionInstructions"};
  t[DisclosureInstructions] = {FieldKind::UInt16, 2, "DisclosureInstructions"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace amend_order

// Execution Report (10), §7.6.7: one bit table shared by all report kinds (accepted, rejected,
// cancelled, amended, trade, ...); which fields appear depends on Exec Type.
namespace exec_report {
enum Bit : std::uint8_t {
  ClOrdId = 0,
  SubmittingBrokerId = 1,
  SecurityId = 2,
  SecurityIdSource = 3,
  SecurityExchange = 4,
  BrokerLocationId = 5,
  TransactTime = 6,
  Side = 7,
  OrigClOrdId = 8,
  OrderId = 9,
  OwningBrokerId = 10,
  OrdType = 11,
  Price = 12,
  OrderQty = 13,
  TimeInForce = 14,
  PositionEffect = 15,
  OrderRestrictions = 16,
  MaxPriceLevels = 17,
  OrderCapacity = 18,
  Text = 19,
  Reason = 20,
  ExecId = 21,
  OrdStatus = 22,
  ExecType = 23,
  CumQty = 24,
  LeavesQty = 25,
  OrderRejectCode = 26,
  LotType = 27,
  ExecRestatementReason = 28,
  CancelRejectCode = 29,
  MatchType = 30,
  CounterpartyBrokerId = 31,
  ExecQty = 32,
  ExecPrice = 33,
  RefExecId = 34,
  OrderCategory = 35,
  AmendRejectCode = 36,
  TradeMatchId = 38,
  AggressorIndicator = 42,
  SmpId = 43,
};

constexpr FieldTable make_fields() {
  namespace s = field_size;
  FieldTable t{};
  t[ClOrdId] = {FieldKind::Alnum, s::kClOrdId, "ClientOrderID"};
  t[SubmittingBrokerId] = {FieldKind::Alnum, s::kBrokerId, "SubmittingBrokerID"};
  t[SecurityId] = {FieldKind::Alnum, s::kSecurityId, "SecurityID"};
  t[SecurityIdSource] = {FieldKind::UInt8, 1, "SecurityIDSource"};
  t[SecurityExchange] = {FieldKind::Alnum, s::kSecurityExchange, "SecurityExchange"};
  t[BrokerLocationId] = {FieldKind::Alnum, s::kBrokerLocationId, "BrokerLocationID"};
  t[TransactTime] = {FieldKind::Alnum, s::kTransactTime, "TransactionTime"};
  t[Side] = {FieldKind::UInt8, 1, "Side"};
  t[OrigClOrdId] = {FieldKind::Alnum, s::kClOrdId, "OriginalClientOrderID"};
  t[OrderId] = {FieldKind::Alnum, s::kOrderId, "OrderID"};
  t[OwningBrokerId] = {FieldKind::Alnum, s::kBrokerId, "OwningBrokerID"};
  t[OrdType] = {FieldKind::UInt8, 1, "OrderType"};
  t[Price] = {FieldKind::Decimal, 8, "Price"};
  t[OrderQty] = {FieldKind::Decimal, 8, "OrderQuantity"};
  t[TimeInForce] = {FieldKind::UInt8, 1, "TIF"};
  t[PositionEffect] = {FieldKind::UInt8, 1, "PositionEffect"};
  t[OrderRestrictions] = {FieldKind::Alnum, s::kOrderRestrictions, "OrderRestrictions"};
  t[MaxPriceLevels] = {FieldKind::UInt8, 1, "MaxPriceLevels"};
  t[OrderCapacity] = {FieldKind::UInt8, 1, "OrderCapacity"};
  t[Text] = {FieldKind::VarAlnum, s::kTextMax, "Text"};
  t[Reason] = {FieldKind::VarAlnum, s::kReasonMax, "Reason"};
  t[ExecId] = {FieldKind::Alnum, s::kExecId, "ExecutionID"};
  t[OrdStatus] = {FieldKind::UInt8, 1, "OrderStatus"};
  t[ExecType] = {FieldKind::Byte, 1, "ExecType"};
  t[CumQty] = {FieldKind::Decimal, 8, "CumulativeQuantity"};
  t[LeavesQty] = {FieldKind::Decimal, 8, "LeavesQuantity"};
  t[OrderRejectCode] = {FieldKind::UInt16, 2, "OrderRejectCode"};
  t[LotType] = {FieldKind::UInt8, 1, "LotType"};
  t[ExecRestatementReason] = {FieldKind::UInt16, 2, "ExecRestatementReason"};
  t[CancelRejectCode] = {FieldKind::UInt16, 2, "CancelRejectCode"};
  t[MatchType] = {FieldKind::UInt8, 1, "MatchType"};
  t[CounterpartyBrokerId] = {FieldKind::Alnum, s::kBrokerId, "CounterpartyBrokerID"};
  t[ExecQty] = {FieldKind::Decimal, 8, "ExecutionQuantity"};
  t[ExecPrice] = {FieldKind::Decimal, 8, "ExecutionPrice"};
  t[RefExecId] = {FieldKind::Alnum, s::kExecId, "ReferenceExecutionID"};
  t[OrderCategory] = {FieldKind::UInt8, 1, "OrderCategory"};
  t[AmendRejectCode] = {FieldKind::UInt16, 2, "AmendRejectCode"};
  t[TradeMatchId] = {FieldKind::Alnum, s::kTradeMatchId, "TradeMatchID"};
  t[AggressorIndicator] = {FieldKind::UInt8, 1, "AggressorIndicator"};
  t[SmpId] = {FieldKind::Alnum, s::kSmpId, "SMPID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace exec_report

// Order Status (§8.2)
enum class OrdStatus : std::uint8_t {
  New = 0,
  PartiallyFilled = 1,
  Filled = 2,
  Cancelled = 4,
  PendingCancel = 6,
  Rejected = 8,
  PendingNew = 10,
  Expired = 12,
  PendingAmend = 14,
};

// Exec Type (§8.2), a single ASCII byte
enum class ExecType : char {
  New = '0',
  Cancel = '4',
  Amend = '5',
  Reject = '8',
  Expire = 'C',
  Trade = 'F',
  TradeCancel = 'H',
  Triggered = 'L',
  CancelReject = 'X',
  AmendReject = 'Y',
};

// --- Session-level messages (§7.5) ---------------------------------------------------------

namespace logon {
enum Bit : std::uint8_t { Password = 0, NewPassword = 1, NextExpectedSeq = 2, SessionStatus = 3, Text = 4, TestMessageIndicator = 5 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[Password] = {FieldKind::Alnum, field_size::kPassword, "Password"};
  t[NewPassword] = {FieldKind::Alnum, field_size::kPassword, "NewPassword"};
  t[NextExpectedSeq] = {FieldKind::UInt32, 4, "NextExpectedMessageSequence"};
  t[SessionStatus] = {FieldKind::UInt8, 1, "SessionStatus"};
  t[Text] = {FieldKind::VarAlnum, field_size::kTextMax, "Text"};
  t[TestMessageIndicator] = {FieldKind::UInt8, 1, "TestMessageIndicator"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace logon

namespace logout {
enum Bit : std::uint8_t { LogoutText = 0, SessionStatus = 1 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[LogoutText] = {FieldKind::VarAlnum, field_size::kReasonMax, "LogoutText"};
  t[SessionStatus] = {FieldKind::UInt8, 1, "SessionStatus"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace logout

namespace heartbeat {
enum Bit : std::uint8_t { RefTestReqId = 0 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[RefTestReqId] = {FieldKind::UInt16, 2, "ReferenceTestRequestID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace heartbeat

namespace test_request {
enum Bit : std::uint8_t { TestReqId = 0 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[TestReqId] = {FieldKind::UInt16, 2, "TestRequestID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace test_request

namespace resend_request {
enum Bit : std::uint8_t { StartSeq = 0, EndSeq = 1 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[StartSeq] = {FieldKind::UInt32, 4, "StartSequence"};
  t[EndSeq] = {FieldKind::UInt32, 4, "EndSequence"};  // 0 = everything from StartSeq on
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace resend_request

namespace reject {
enum Bit : std::uint8_t { MessageRejectCode = 0, Reason = 1, RefMsgType = 2, RefFieldName = 3, RefSeqNum = 4, ClOrdId = 5 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[MessageRejectCode] = {FieldKind::UInt16, 2, "MessageRejectCode"};
  t[Reason] = {FieldKind::VarAlnum, field_size::kReasonMax, "Reason"};
  t[RefMsgType] = {FieldKind::UInt8, 1, "ReferenceMessageType"};
  t[RefFieldName] = {FieldKind::Alnum, field_size::kRefFieldName, "ReferenceFieldName"};
  t[RefSeqNum] = {FieldKind::UInt32, 4, "ReferenceSequenceNumber"};
  t[ClOrdId] = {FieldKind::Alnum, field_size::kClOrdId, "ClientOrderID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace reject

namespace sequence_reset {
enum Bit : std::uint8_t { GapFill = 0, NewSeqNo = 1 };
inline constexpr char kGapFill = 'Y';
inline constexpr char kReset = 'N';  // default when GapFill is absent; only OCG-C may use it
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[GapFill] = {FieldKind::Byte, 1, "GapFill"};
  t[NewSeqNo] = {FieldKind::UInt32, 4, "NewSequenceNumber"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace sequence_reset

namespace business_reject {
enum Bit : std::uint8_t { BusinessRejectCode = 0, Reason = 1, RefMsgType = 2, RefFieldName = 3, RefSeqNum = 4, BusinessRejectRefId = 5 };
constexpr FieldTable make_fields() {
  FieldTable t{};
  t[BusinessRejectCode] = {FieldKind::UInt16, 2, "BusinessRejectCode"};
  t[Reason] = {FieldKind::VarAlnum, field_size::kReasonMax, "Reason"};
  t[RefMsgType] = {FieldKind::UInt8, 1, "ReferenceMessageType"};
  t[RefFieldName] = {FieldKind::Alnum, field_size::kRefFieldName, "ReferenceFieldName"};
  t[RefSeqNum] = {FieldKind::UInt32, 4, "ReferenceSequenceNumber"};
  t[BusinessRejectRefId] = {FieldKind::Alnum, field_size::kClOrdId, "BusinessRejectReferenceID"};
  return t;
}
inline constexpr FieldTable kFields = make_fields();
}  // namespace business_reject

// Session Status (§8.2)
enum class SessionStatus : std::uint8_t {
  Active = 0,
  PasswordChanged = 1,
  PasswordDueToExpire = 2,
  NewPasswordNotCompliant = 3,
  LogoutComplete = 4,
  InvalidUsernameOrPassword = 5,
  AccountLocked = 6,
  LogonsNotAllowed = 7,
  PasswordExpired = 8,
  PasswordChangeRequired = 100,
  Other = 101,
};

inline const FieldTable* fields_for(MsgType t) {
  switch (t) {
    case MsgType::Heartbeat: return &heartbeat::kFields;
    case MsgType::TestRequest: return &test_request::kFields;
    case MsgType::ResendRequest: return &resend_request::kFields;
    case MsgType::Reject: return &reject::kFields;
    case MsgType::SequenceReset: return &sequence_reset::kFields;
    case MsgType::Logon: return &logon::kFields;
    case MsgType::Logout: return &logout::kFields;
    case MsgType::BusinessMessageReject: return &business_reject::kFields;
    case MsgType::ExecutionReport: return &exec_report::kFields;
    case MsgType::NewOrder: return &new_order::kFields;
    case MsgType::AmendOrder: return &amend_order::kFields;
    case MsgType::CancelOrder: return &cancel_order::kFields;
    default: return nullptr;
  }
}

// Session-level messages: skipped with a gap fill instead of being replayed (§5.6).
inline bool is_admin(MsgType t) {
  switch (t) {
    case MsgType::Logon:
    case MsgType::Logout:
    case MsgType::Heartbeat:
    case MsgType::TestRequest:
    case MsgType::ResendRequest:
    case MsgType::SequenceReset:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Checksum

inline std::uint32_t checksum(const std::uint8_t* msg, std::size_t len_without_trailer) {
#if defined(__SSE4_2__)
  return crc32c_hw(0, msg, len_without_trailer);
#else
  return crc32c_slice8(0, msg, len_without_trailer);
#endif
}

inline bool verify_checksum(const std::uint8_t* msg, std::size_t len) {
  if (len < hdr::kSize + kTrailerSize) return false;
  return checksum(msg, len - kTrailerSize) == load_le<std::uint32_t>(msg + len - kTrailerSize);
}

// ---------------------------------------------------------------------------------------------
// Text formatting helpers

namespace detail {
inline constexpr char kDigitPairs[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline void put2(char* out, std::uint32_t v) { std::memcpy(out, kDigitPairs + 2 * v, 2); }
}  // namespace detail

// "HH:MM:SS.ssssss" (15 chars) from microseconds since UTC midnight.
inline void format_time_of_day(char* out, std::uint64_t us_of_day) {
  const auto us = static_cast<std::uint32_t>(us_of_day % 1'000'000);
  const auto secs = static_cast<std::uint32_t>(us_of_day / 1'000'000);
  detail::put2(out + 0, secs / 3600);
  out[2] = ':';
  detail::put2(out + 3, secs / 60 % 60);
  out[5] = ':';
  detail::put2(out + 6, secs % 60);
  out[8] = '.';
  detail::put2(out + 9, us / 10000);
  detail::put2(out + 11, us / 100 % 100);
  detail::put2(out + 13, us % 100);
}

// "YYYYMMDD" (8 chars).
inline void format_date(char* out, std::uint32_t yyyymmdd) {
  detail::put2(out + 0, yyyymmdd / 1000000);
  detail::put2(out + 2, yyyymmdd / 10000 % 100);
  detail::put2(out + 4, yyyymmdd / 100 % 100);
  detail::put2(out + 6, yyyymmdd % 100);
}

// "YYYYMMDD-HH:MM:SS.ssssss" (24 chars, no NUL).
inline void format_transact_time(char* out, std::uint32_t yyyymmdd, std::uint64_t us_of_day) {
  format_date(out, yyyymmdd);
  out[8] = '-';
  format_time_of_day(out + 9, us_of_day);
}

// Client Order ID: numeric 1..99,999,999, no leading zeroes (§6.6.3.1).
inline constexpr std::uint32_t kMaxClOrdId = 99'999'999;

// ---------------------------------------------------------------------------------------------
// Writer: builds any message field by field. Fields must be added in increasing bit order.

class Writer {
 public:
  Writer(std::uint8_t* buf, MsgType type, std::uint32_t seq, std::string_view comp_id,
         bool poss_dup = false, bool poss_resend = false)
      : buf_(buf), p_(buf + hdr::kSize) {
    buf_[hdr::kStart] = kStx;
    buf_[hdr::kMsgType] = static_cast<std::uint8_t>(type);
    store_le<std::uint32_t>(buf_ + hdr::kSeqNum, seq);
    buf_[hdr::kPossDup] = poss_dup;
    buf_[hdr::kPossResend] = poss_resend;
    put_alnum(buf_ + hdr::kCompId, comp_id, hdr::kCompIdSize);
    std::memset(buf_ + hdr::kPresenceMap, 0, hdr::kPresenceMapSize);
  }

  Writer& alnum(unsigned bit, std::string_view v, std::size_t size) {
    mark(bit);
    put_alnum(p_, v, size);
    p_ += size;
    return *this;
  }
  Writer& var_alnum(unsigned bit, std::string_view v) {
    mark(bit);
    const auto len = static_cast<std::uint16_t>(v.size() + 1);  // length includes the NUL
    store_le<std::uint16_t>(p_, len);
    std::memcpy(p_ + 2, v.data(), v.size());
    p_[2 + v.size()] = 0;
    p_ += 2 + len;
    return *this;
  }
  Writer& u8(unsigned bit, std::uint8_t v) {
    mark(bit);
    *p_++ = v;
    return *this;
  }
  Writer& u16(unsigned bit, std::uint16_t v) { return scalar(bit, v); }
  Writer& u32(unsigned bit, std::uint32_t v) { return scalar(bit, v); }
  Writer& decimal(unsigned bit, std::int64_t v) { return scalar(bit, v); }

  // Reserve an Alnum field and return its bytes (zero filled) for the caller to format into.
  char* alnum_slot(unsigned bit, std::size_t size) {
    mark(bit);
    std::memset(p_, 0, size);
    auto* slot = reinterpret_cast<char*>(p_);
    p_ += size;
    return slot;
  }

  // Fill in Length and Checksum; returns the message length.
  std::size_t finish() {
    const auto len = static_cast<std::size_t>(p_ - buf_) + kTrailerSize;
    store_le<std::uint16_t>(buf_ + hdr::kLength, static_cast<std::uint16_t>(len));
    store_le<std::uint32_t>(p_, checksum(buf_, len - kTrailerSize));
    return len;
  }

  std::size_t body_offset() const { return static_cast<std::size_t>(p_ - buf_); }

 private:
  static void put_alnum(std::uint8_t* dst, std::string_view v, std::size_t size) {
    const auto n = v.size() < size ? v.size() : size - 1;
    std::memcpy(dst, v.data(), n);
    std::memset(dst + n, 0, size - n);
  }
  void mark(unsigned bit) {
    assert(static_cast<int>(bit) > last_bit_ && bit < 256);
    last_bit_ = static_cast<int>(bit);
    buf_[hdr::kPresenceMap + bit / 8] |= static_cast<std::uint8_t>(0x80u >> (bit % 8));
  }
  template <class T>
  Writer& scalar(unsigned bit, T v) {
    mark(bit);
    store_le<T>(p_, v);
    p_ += sizeof(T);
    return *this;
  }

  std::uint8_t* buf_;
  std::uint8_t* p_;
  int last_bit_ = -1;
};

// ---------------------------------------------------------------------------------------------
// Reader

struct Header {
  MsgType type;
  std::uint16_t length;
  std::uint32_t seq;
  std::uint8_t poss_dup;
  std::uint8_t poss_resend;
  std::string_view comp_id;
};

inline std::string_view read_alnum(const std::uint8_t* p, std::size_t size) {
  const auto* c = reinterpret_cast<const char*>(p);
  const void* nul = std::memchr(c, 0, size);
  return {c, nul ? static_cast<std::size_t>(static_cast<const char*>(nul) - c) : size};
}

inline bool presence_bit(const std::uint8_t* msg, unsigned bit) {
  return msg[hdr::kPresenceMap + bit / 8] & (0x80u >> (bit % 8));
}

inline Header read_header(const std::uint8_t* msg) {
  return {static_cast<MsgType>(msg[hdr::kMsgType]),
          load_le<std::uint16_t>(msg + hdr::kLength),
          load_le<std::uint32_t>(msg + hdr::kSeqNum),
          msg[hdr::kPossDup],
          msg[hdr::kPossResend],
          read_alnum(msg + hdr::kCompId, hdr::kCompIdSize)};
}

struct FieldRef {
  unsigned bit;
  const FieldDef* def;
  const std::uint8_t* data;  // VarAlnum: points at the content (after the UInt16 length)
  std::size_t size;          // bytes of data
};

// Frame check: STX, Length == len, checksum. Returns false if the message must be dropped (and,
// per §4.8, the connection with it).
inline bool valid_frame(const std::uint8_t* msg, std::size_t len) {
  if (len < hdr::kSize + kTrailerSize || msg[hdr::kStart] != kStx) return false;
  if (load_le<std::uint16_t>(msg + hdr::kLength) != len) return false;
  return verify_checksum(msg, len);
}

// Calls f(FieldRef) for each present body field of a message whose frame is already validated.
// Walks the presence map 64 bits at a time with count-leading-zeros instead of testing all 256
// bits. Returns false on an unknown bit or a field running past the trailer.
template <class F>
bool for_each_field_unchecked(const std::uint8_t* msg, std::size_t len, const FieldTable& defs, F&& f) {
  const std::uint8_t* p = msg + hdr::kSize;
  const std::uint8_t* end = msg + len - kTrailerSize;
  for (unsigned w = 0; w < 4; ++w) {
    // bit 0 is the MSB of the first byte, so a big-endian load puts bit order = MSB-first order
    std::uint64_t bits = __builtin_bswap64(load_le<std::uint64_t>(msg + hdr::kPresenceMap + 8 * w));
    while (bits) {
      const unsigned i = static_cast<unsigned>(std::countl_zero(bits));
      bits &= ~(std::uint64_t{1} << (63 - i));
      const unsigned bit = 64 * w + i;
      const FieldDef& d = defs[bit];
      std::size_t size = d.size;
      const std::uint8_t* data = p;
      if (d.kind == FieldKind::VarAlnum) {
        if (end - p < 2) return false;
        size = load_le<std::uint16_t>(p);
        data = p + 2;
        if (static_cast<std::size_t>(end - data) < size) return false;
        p = data + size;
      } else {
        if (d.kind == FieldKind::Unused || static_cast<std::size_t>(end - p) < size) return false;
        p += size;
      }
      f(FieldRef{bit, &d, data, size});
    }
  }
  return p == end;
}

// Frame check + for_each_field_unchecked.
template <class F>
bool for_each_field(const std::uint8_t* msg, std::size_t len, const FieldTable& defs, F&& f) {
  return valid_frame(msg, len) && for_each_field_unchecked(msg, len, defs, std::forward<F>(f));
}

// Field value accessors for FieldRef
inline std::uint8_t as_u8(const FieldRef& f) { return f.data[0]; }
inline std::uint16_t as_u16(const FieldRef& f) { return load_le<std::uint16_t>(f.data); }
inline std::uint32_t as_u32(const FieldRef& f) { return load_le<std::uint32_t>(f.data); }
inline std::int64_t as_decimal(const FieldRef& f) { return load_le<std::int64_t>(f.data); }
inline std::string_view as_str(const FieldRef& f) { return read_alnum(f.data, f.size); }

}  // namespace obl::gw::ocgc
