#pragma once
// NYSE XDP Integrated Feed (Pillar, client spec v2.5) wire layouts.
//
// Offsets are taken from the published client specification as encoded by the
// Open Markets Initiative Wireshark dissector
// (Nyse_NyseEquities_IntegratedFeed_Pillar_v2_5_h_Dissector.lua).
// All integers are little-endian. Every message starts with
//   MsgSize (u16, includes the 4-byte header) | MsgType (u16)
// which is the same framing HKEX OMD-C uses, so the decoder skeleton carries over.
//
// Encoders exist for the synthetic data generator and for tests; they use the
// same offset constants as the decoders so the two cannot drift apart.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "obl/common.hpp"

namespace obl::xdp {

// ---------------------------------------------------------------- packet header
// PktSize u16 | DeliveryFlag u8 | NumberMsgs u8 | SeqNum u32 | SendTime u32 | SendTimeNS u32
inline constexpr std::size_t kPacketHeaderSize = 16;

struct PacketHeader {
  std::uint16_t pkt_size;
  std::uint8_t delivery_flag;
  std::uint8_t num_msgs;
  std::uint32_t seq_num;     // sequence number of the first message in the packet
  std::uint32_t send_time_s;
  std::uint32_t send_time_ns;
};

inline PacketHeader decode_packet_header(const std::uint8_t* p) {
  return {load_le<std::uint16_t>(p + 0), p[2], p[3], load_le<std::uint32_t>(p + 4),
          load_le<std::uint32_t>(p + 8), load_le<std::uint32_t>(p + 12)};
}

inline void encode_packet_header(std::uint8_t* p, const PacketHeader& h) {
  store_le<std::uint16_t>(p + 0, h.pkt_size);
  p[2] = h.delivery_flag;
  p[3] = h.num_msgs;
  store_le<std::uint32_t>(p + 4, h.seq_num);
  store_le<std::uint32_t>(p + 8, h.send_time_s);
  store_le<std::uint32_t>(p + 12, h.send_time_ns);
}

// ---------------------------------------------------------------- message types
inline constexpr std::size_t kMsgHeaderSize = 4;

enum MsgType : std::uint16_t {
  kSequenceNumberReset = 1,
  kSourceTimeReference = 2,
  kSymbolIndexMapping = 3,
  kSymbolClear = 32,
  kSecurityStatus = 34,
  kAddOrder = 100,
  kModifyOrder = 101,
  kDeleteOrder = 102,
  kOrderExecution = 103,
  kReplaceOrder = 104,
  kImbalance = 105,
  kAddOrderRefresh = 106,
  kNonDisplayedTrade = 110,
  kCrossTrade = 111,
  kTradeCancel = 112,
  kCrossCorrection = 113,
  kRetailPriceImprovement = 114,
};

inline constexpr std::string_view msg_type_name(std::uint16_t t) {
  switch (t) {
    case kSequenceNumberReset: return "SequenceNumberReset";
    case kSourceTimeReference: return "SourceTimeReference";
    case kSymbolIndexMapping: return "SymbolIndexMapping";
    case kSymbolClear: return "SymbolClear";
    case kSecurityStatus: return "SecurityStatus";
    case kAddOrder: return "AddOrder";
    case kModifyOrder: return "ModifyOrder";
    case kDeleteOrder: return "DeleteOrder";
    case kOrderExecution: return "OrderExecution";
    case kReplaceOrder: return "ReplaceOrder";
    case kImbalance: return "Imbalance";
    case kAddOrderRefresh: return "AddOrderRefresh";
    case kNonDisplayedTrade: return "NonDisplayedTrade";
    case kCrossTrade: return "CrossTrade";
    case kTradeCancel: return "TradeCancel";
    case kCrossCorrection: return "CrossCorrection";
    case kRetailPriceImprovement: return "RetailPriceImprovement";
    default: return "Unknown";
  }
}

// Minimum on-wire size (MsgSize) of the messages we decode. A message may be
// longer than this (forward-compatible growth); shorter means malformed.
inline constexpr std::uint16_t kSequenceNumberResetSize = 14;
inline constexpr std::uint16_t kSymbolIndexMappingSize = 44;
inline constexpr std::uint16_t kSymbolClearSize = 20;
inline constexpr std::uint16_t kAddOrderSize = 39;
inline constexpr std::uint16_t kModifyOrderSize = 35;
inline constexpr std::uint16_t kDeleteOrderSize = 25;
inline constexpr std::uint16_t kOrderExecutionSize = 42;
inline constexpr std::uint16_t kReplaceOrderSize = 42;
inline constexpr std::uint16_t kAddOrderRefreshSize = 43;

inline constexpr std::uint16_t min_msg_size(std::uint16_t t) {
  switch (t) {
    case kSequenceNumberReset: return kSequenceNumberResetSize;
    case kSymbolIndexMapping: return kSymbolIndexMappingSize;
    case kSymbolClear: return kSymbolClearSize;
    case kAddOrder: return kAddOrderSize;
    case kModifyOrder: return kModifyOrderSize;
    case kDeleteOrder: return kDeleteOrderSize;
    case kOrderExecution: return kOrderExecutionSize;
    case kReplaceOrder: return kReplaceOrderSize;
    case kAddOrderRefresh: return kAddOrderRefreshSize;
    default: return kMsgHeaderSize;
  }
}

inline Side decode_side(std::uint8_t c) { return c == 'S' ? Side::Sell : Side::Buy; }
inline std::uint8_t encode_side(Side s) { return s == Side::Sell ? 'S' : 'B'; }

inline void encode_msg_header(std::uint8_t* p, std::uint16_t size, std::uint16_t type) {
  store_le<std::uint16_t>(p + 0, size);
  store_le<std::uint16_t>(p + 2, type);
}

// ---------------------------------------------------------------- 1: Sequence Number Reset
// SourceTime u32 @4 | SourceTimeNS u32 @8 | ProductID u8 @12 | ChannelID u8 @13
struct SequenceNumberReset {
  std::uint32_t source_time;
  std::uint32_t source_time_ns;
  std::uint8_t product_id;
  std::uint8_t channel_id;
};

inline SequenceNumberReset decode_sequence_number_reset(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4), load_le<std::uint32_t>(p + 8), p[12], p[13]};
}

inline std::size_t encode(std::uint8_t* p, const SequenceNumberReset& m) {
  encode_msg_header(p, kSequenceNumberResetSize, kSequenceNumberReset);
  store_le(p + 4, m.source_time);
  store_le(p + 8, m.source_time_ns);
  p[12] = m.product_id;
  p[13] = m.channel_id;
  return kSequenceNumberResetSize;
}

// ---------------------------------------------------------------- 3: Symbol Index Mapping
// SymbolIndex u32 @4 | Symbol char[11] @8 | Reserved u8 @19 | MarketID u16 @20 | SystemID u8 @22
// ExchangeCode char @23 | PriceScaleCode u8 @24 | SecurityType char @25 | LotSize u16 @26
// PrevClosePrice u32 @28 | PrevCloseVolume u32 @32 | PriceResolution u8 @36 | RoundLot char @37
// MPV u16 @38 | UnitOfTrade u16 @40 | LateCloseEligible u8 @42 | EthEligible u8 @43
struct SymbolIndexMapping {
  std::uint32_t symbol_index;
  char symbol[11];
  std::uint16_t market_id;
  std::uint8_t system_id;
  char exchange_code;
  std::uint8_t price_scale_code;  // price = raw / 10^price_scale_code
  char security_type;
  std::uint16_t lot_size;
  std::uint32_t prev_close_price;
  std::uint32_t prev_close_volume;
  std::uint8_t price_resolution;
  char round_lot;
  std::uint16_t mpv;
  std::uint16_t unit_of_trade;

  std::string_view symbol_view() const {
    std::size_t n = 0;
    while (n < sizeof(symbol) && symbol[n] != '\0' && symbol[n] != ' ') ++n;
    return {symbol, n};
  }
};

inline SymbolIndexMapping decode_symbol_index_mapping(const std::uint8_t* p) {
  SymbolIndexMapping m{};
  m.symbol_index = load_le<std::uint32_t>(p + 4);
  std::memcpy(m.symbol, p + 8, sizeof(m.symbol));
  m.market_id = load_le<std::uint16_t>(p + 20);
  m.system_id = p[22];
  m.exchange_code = static_cast<char>(p[23]);
  m.price_scale_code = p[24];
  m.security_type = static_cast<char>(p[25]);
  m.lot_size = load_le<std::uint16_t>(p + 26);
  m.prev_close_price = load_le<std::uint32_t>(p + 28);
  m.prev_close_volume = load_le<std::uint32_t>(p + 32);
  m.price_resolution = p[36];
  m.round_lot = static_cast<char>(p[37]);
  m.mpv = load_le<std::uint16_t>(p + 38);
  m.unit_of_trade = load_le<std::uint16_t>(p + 40);
  return m;
}

inline std::size_t encode(std::uint8_t* p, const SymbolIndexMapping& m) {
  std::memset(p, 0, kSymbolIndexMappingSize);
  encode_msg_header(p, kSymbolIndexMappingSize, kSymbolIndexMapping);
  store_le(p + 4, m.symbol_index);
  std::memcpy(p + 8, m.symbol, sizeof(m.symbol));
  store_le(p + 20, m.market_id);
  p[22] = m.system_id;
  p[23] = static_cast<std::uint8_t>(m.exchange_code);
  p[24] = m.price_scale_code;
  p[25] = static_cast<std::uint8_t>(m.security_type);
  store_le(p + 26, m.lot_size);
  store_le(p + 28, m.prev_close_price);
  store_le(p + 32, m.prev_close_volume);
  p[36] = m.price_resolution;
  p[37] = static_cast<std::uint8_t>(m.round_lot);
  store_le(p + 38, m.mpv);
  store_le(p + 40, m.unit_of_trade);
  return kSymbolIndexMappingSize;
}

// ---------------------------------------------------------------- 32: Symbol Clear
// SourceTime u32 @4 | SourceTimeNS u32 @8 | SymbolIndex u32 @12 | NextSourceSeqNum u32 @16
struct SymbolClear {
  std::uint32_t source_time;
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t next_source_seq_num;
};

inline SymbolClear decode_symbol_clear(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4), load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint32_t>(p + 16)};
}

inline std::size_t encode(std::uint8_t* p, const SymbolClear& m) {
  encode_msg_header(p, kSymbolClearSize, kSymbolClear);
  store_le(p + 4, m.source_time);
  store_le(p + 8, m.source_time_ns);
  store_le(p + 12, m.symbol_index);
  store_le(p + 16, m.next_source_seq_num);
  return kSymbolClearSize;
}

// Common prefix of all order messages:
// SourceTimeNS u32 @4 | SymbolIndex u32 @8 | SymbolSeqNum u32 @12 | OrderID u64 @16

// ---------------------------------------------------------------- 100: Add Order
// ... Price u32 @24 | Volume u32 @28 | Side char @32 | FirmID char[5] @33 | Reserved @38
struct AddOrder {
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t symbol_seq_num;
  std::uint64_t order_id;
  std::uint32_t price;
  std::uint32_t volume;
  Side side;
};

inline AddOrder decode_add_order(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4),  load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint64_t>(p + 16),
          load_le<std::uint32_t>(p + 24), load_le<std::uint32_t>(p + 28),
          decode_side(p[32])};
}

inline std::size_t encode(std::uint8_t* p, const AddOrder& m) {
  std::memset(p, 0, kAddOrderSize);
  encode_msg_header(p, kAddOrderSize, kAddOrder);
  store_le(p + 4, m.source_time_ns);
  store_le(p + 8, m.symbol_index);
  store_le(p + 12, m.symbol_seq_num);
  store_le(p + 16, m.order_id);
  store_le(p + 24, m.price);
  store_le(p + 28, m.volume);
  p[32] = encode_side(m.side);
  return kAddOrderSize;
}

// ---------------------------------------------------------------- 106: Add Order Refresh
// SourceTime u32 @4 | SourceTimeNS u32 @8 | SymbolIndex u32 @12 | SymbolSeqNum u32 @16
// OrderID u64 @20 | Price u32 @28 | Volume u32 @32 | Side char @36 | FirmID char[5] @37 | Reserved @42
inline AddOrder decode_add_order_refresh(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 8),  load_le<std::uint32_t>(p + 12),
          load_le<std::uint32_t>(p + 16), load_le<std::uint64_t>(p + 20),
          load_le<std::uint32_t>(p + 28), load_le<std::uint32_t>(p + 32),
          decode_side(p[36])};
}

// ---------------------------------------------------------------- 101: Modify Order
// ... Price u32 @24 | Volume u32 @28 | PositionChange u8 @32 | Side char @33 | Reserved @34
struct ModifyOrder {
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t symbol_seq_num;
  std::uint64_t order_id;
  std::uint32_t price;
  std::uint32_t volume;
  std::uint8_t position_change;  // 1 = order lost its queue position
  Side side;
};

inline ModifyOrder decode_modify_order(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4),  load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint64_t>(p + 16),
          load_le<std::uint32_t>(p + 24), load_le<std::uint32_t>(p + 28),
          p[32],                          decode_side(p[33])};
}

inline std::size_t encode(std::uint8_t* p, const ModifyOrder& m) {
  std::memset(p, 0, kModifyOrderSize);
  encode_msg_header(p, kModifyOrderSize, kModifyOrder);
  store_le(p + 4, m.source_time_ns);
  store_le(p + 8, m.symbol_index);
  store_le(p + 12, m.symbol_seq_num);
  store_le(p + 16, m.order_id);
  store_le(p + 24, m.price);
  store_le(p + 28, m.volume);
  p[32] = m.position_change;
  p[33] = encode_side(m.side);
  return kModifyOrderSize;
}

// ---------------------------------------------------------------- 102: Delete Order
// ... Reserved @24
struct DeleteOrder {
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t symbol_seq_num;
  std::uint64_t order_id;
};

inline DeleteOrder decode_delete_order(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4), load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint64_t>(p + 16)};
}

inline std::size_t encode(std::uint8_t* p, const DeleteOrder& m) {
  std::memset(p, 0, kDeleteOrderSize);
  encode_msg_header(p, kDeleteOrderSize, kDeleteOrder);
  store_le(p + 4, m.source_time_ns);
  store_le(p + 8, m.symbol_index);
  store_le(p + 12, m.symbol_seq_num);
  store_le(p + 16, m.order_id);
  return kDeleteOrderSize;
}

// ---------------------------------------------------------------- 103: Order Execution
// ... TradeID u32 @24 | Price u32 @28 | Volume u32 @32 | PrintableFlag u8 @36 | Reserved @37
// TradeCond1..4 char @38..41
struct OrderExecution {
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t symbol_seq_num;
  std::uint64_t order_id;
  std::uint32_t trade_id;
  std::uint32_t price;
  std::uint32_t volume;  // executed quantity
  std::uint8_t printable_flag;
};

inline OrderExecution decode_order_execution(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4),  load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint64_t>(p + 16),
          load_le<std::uint32_t>(p + 24), load_le<std::uint32_t>(p + 28),
          load_le<std::uint32_t>(p + 32), p[36]};
}

inline std::size_t encode(std::uint8_t* p, const OrderExecution& m) {
  std::memset(p, 0, kOrderExecutionSize);
  encode_msg_header(p, kOrderExecutionSize, kOrderExecution);
  store_le(p + 4, m.source_time_ns);
  store_le(p + 8, m.symbol_index);
  store_le(p + 12, m.symbol_seq_num);
  store_le(p + 16, m.order_id);
  store_le(p + 24, m.trade_id);
  store_le(p + 28, m.price);
  store_le(p + 32, m.volume);
  p[36] = m.printable_flag;
  std::memset(p + 38, ' ', 4);
  return kOrderExecutionSize;
}

// ---------------------------------------------------------------- 104: Replace Order
// ... NewOrderID u64 @24 | Price u32 @32 | Volume u32 @36 | Side char @40 | Reserved @41
struct ReplaceOrder {
  std::uint32_t source_time_ns;
  std::uint32_t symbol_index;
  std::uint32_t symbol_seq_num;
  std::uint64_t order_id;
  std::uint64_t new_order_id;
  std::uint32_t price;
  std::uint32_t volume;
  Side side;
};

inline ReplaceOrder decode_replace_order(const std::uint8_t* p) {
  return {load_le<std::uint32_t>(p + 4),  load_le<std::uint32_t>(p + 8),
          load_le<std::uint32_t>(p + 12), load_le<std::uint64_t>(p + 16),
          load_le<std::uint64_t>(p + 24), load_le<std::uint32_t>(p + 32),
          load_le<std::uint32_t>(p + 36), decode_side(p[40])};
}

inline std::size_t encode(std::uint8_t* p, const ReplaceOrder& m) {
  std::memset(p, 0, kReplaceOrderSize);
  encode_msg_header(p, kReplaceOrderSize, kReplaceOrder);
  store_le(p + 4, m.source_time_ns);
  store_le(p + 8, m.symbol_index);
  store_le(p + 12, m.symbol_seq_num);
  store_le(p + 16, m.order_id);
  store_le(p + 24, m.new_order_id);
  store_le(p + 32, m.price);
  store_le(p + 36, m.volume);
  p[40] = encode_side(m.side);
  return kReplaceOrderSize;
}

}  // namespace obl::xdp
