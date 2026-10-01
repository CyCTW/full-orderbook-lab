#pragma once
// Packet decoder + sequence arbitration for the XDP Integrated Feed.
//
// The visitor receives typed messages. Derive from NullVisitor and hide the
// callbacks you care about; dispatch is static (template), no virtual calls.

#include <array>
#include <cstdint>
#include <unordered_map>

#include "obl/xdp/messages.hpp"

namespace obl::xdp {

struct NullVisitor {
  void on_sequence_reset(const SequenceNumberReset&) {}
  void on_symbol_mapping(const SymbolIndexMapping&) {}
  void on_symbol_clear(const SymbolClear&) {}
  void on_add(const AddOrder&) {}
  void on_modify(const ModifyOrder&) {}
  void on_delete(const DeleteOrder&) {}
  void on_execution(const OrderExecution&) {}
  void on_replace(const ReplaceOrder&) {}
  void on_other(std::uint16_t /*type*/, const std::uint8_t* /*msg*/, std::uint16_t /*size*/) {}
};

struct DecodeStats {
  std::uint64_t packets = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t messages = 0;
  std::uint64_t malformed_packets = 0;
  std::uint64_t short_messages = 0;  // MsgSize below the spec minimum for its type
  std::uint64_t duplicate_packets = 0;
  std::uint64_t gaps = 0;
  std::uint64_t missing_messages = 0;
  std::array<std::uint64_t, 256> by_type{};
};

// Decodes the messages of one packet, starting at message index `skip`
// (used to drop the already-seen prefix of a partially duplicated packet).
// Returns false if the packet is structurally invalid.
template <class Visitor>
bool decode_messages(const std::uint8_t* data, std::size_t len, Visitor& v, DecodeStats& st,
                     unsigned skip = 0) {
  if (len < kPacketHeaderSize) return false;
  const PacketHeader h = decode_packet_header(data);
  if (h.pkt_size < kPacketHeaderSize || h.pkt_size > len) return false;
  const std::uint8_t* p = data + kPacketHeaderSize;
  const std::uint8_t* end = data + h.pkt_size;
  for (unsigned i = 0; i < h.num_msgs; ++i) {
    if (end - p < static_cast<std::ptrdiff_t>(kMsgHeaderSize)) return false;
    const std::uint16_t size = load_le<std::uint16_t>(p);
    const std::uint16_t type = load_le<std::uint16_t>(p + 2);
    if (size < kMsgHeaderSize || size > end - p) return false;
    if (i >= skip) {
      ++st.messages;
      ++st.by_type[type & 0xFF];
      if (size < min_msg_size(type)) {
        ++st.short_messages;
      } else {
        switch (type) {
          case kAddOrder: v.on_add(decode_add_order(p)); break;
          case kModifyOrder: v.on_modify(decode_modify_order(p)); break;
          case kDeleteOrder: v.on_delete(decode_delete_order(p)); break;
          case kOrderExecution: v.on_execution(decode_order_execution(p)); break;
          case kReplaceOrder: v.on_replace(decode_replace_order(p)); break;
          case kAddOrderRefresh: v.on_add(decode_add_order_refresh(p)); break;
          case kSymbolIndexMapping: v.on_symbol_mapping(decode_symbol_index_mapping(p)); break;
          case kSymbolClear: v.on_symbol_clear(decode_symbol_clear(p)); break;
          case kSequenceNumberReset: v.on_sequence_reset(decode_sequence_number_reset(p)); break;
          default: v.on_other(type, p, size); break;
        }
      }
    }
    p += size;
  }
  return true;
}

// Per-stream (multicast group:port) sequence tracking with A/B line arbitration:
// whichever copy of a packet arrives first is processed, the other is dropped.
class SequenceTracker {
 public:
  struct Decision {
    bool process;
    unsigned skip;  // leading messages already seen
  };

  // `starts_with_reset`: the first message is a Sequence Number Reset, which the
  // feed sends with SeqNum 1 at start of day or after a publisher failover.
  Decision on_packet(std::uint64_t stream, std::uint32_t seq, std::uint8_t num_msgs,
                     bool starts_with_reset, DecodeStats& st) {
    if (num_msgs == 0) {
      ++st.heartbeats;
      return {false, 0};
    }
    std::uint32_t& next = next_[stream];
    const std::uint32_t last = seq + num_msgs;  // one past the last message in this packet
    if (next == 0 || seq == next || (starts_with_reset && seq == 1 && next != last)) {
      // first packet, in order, or a sequence reset
      next = last;
      return {true, 0};
    }
    if (seq > next) {
      ++st.gaps;
      st.missing_messages += seq - next;
      next = last;
      return {true, 0};
    }
    if (last <= next) {
      ++st.duplicate_packets;
      return {false, 0};
    }
    const unsigned skip = next - seq;  // partial overlap
    next = last;
    return {true, skip};
  }

  void reset() { next_.clear(); }

 private:
  std::unordered_map<std::uint64_t, std::uint32_t> next_;
};

// Convenience: decode one UDP payload of a stream with arbitration.
template <class Visitor>
void process_datagram(SequenceTracker& seq, std::uint64_t stream, const std::uint8_t* data,
                      std::size_t len, Visitor& v, DecodeStats& st) {
  ++st.packets;
  if (len < kPacketHeaderSize) {
    ++st.malformed_packets;
    return;
  }
  const PacketHeader h = decode_packet_header(data);
  const bool starts_with_reset =
      h.num_msgs > 0 && len >= kPacketHeaderSize + kMsgHeaderSize &&
      load_le<std::uint16_t>(data + kPacketHeaderSize + 2) == kSequenceNumberReset;
  const auto d = seq.on_packet(stream, h.seq_num, h.num_msgs, starts_with_reset, st);
  if (!d.process) return;
  if (!decode_messages(data, len, v, st, d.skip)) ++st.malformed_packets;
}

}  // namespace obl::xdp
