// Wire format, sequence arbitration and pcap roundtrip tests.

#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "obl/pcap/pcap.hpp"
#include "obl/xdp/decoder.hpp"

using namespace obl;
using namespace obl::xdp;

namespace {

struct Collect : NullVisitor {
  std::vector<AddOrder> adds;
  std::vector<ModifyOrder> mods;
  std::vector<DeleteOrder> dels;
  std::vector<OrderExecution> execs;
  std::vector<ReplaceOrder> reps;
  std::vector<SymbolIndexMapping> maps;
  std::vector<SymbolClear> clears;
  std::vector<std::uint16_t> others;
  void on_add(const AddOrder& m) { adds.push_back(m); }
  void on_modify(const ModifyOrder& m) { mods.push_back(m); }
  void on_delete(const DeleteOrder& m) { dels.push_back(m); }
  void on_execution(const OrderExecution& m) { execs.push_back(m); }
  void on_replace(const ReplaceOrder& m) { reps.push_back(m); }
  void on_symbol_mapping(const SymbolIndexMapping& m) { maps.push_back(m); }
  void on_symbol_clear(const SymbolClear& m) { clears.push_back(m); }
  void on_other(std::uint16_t t, const std::uint8_t*, std::uint16_t) { others.push_back(t); }
};

struct PacketBuf {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(kPacketHeaderSize);
  std::uint8_t n = 0;
  template <class M>
  PacketBuf& add(const M& m) {
    std::uint8_t tmp[64];
    const std::size_t len = encode(tmp, m);
    bytes.insert(bytes.end(), tmp, tmp + len);
    ++n;
    return *this;
  }
  std::vector<std::uint8_t> finish(std::uint32_t seq) {
    encode_packet_header(bytes.data(), {static_cast<std::uint16_t>(bytes.size()), 0, n, seq, 1, 2});
    return bytes;
  }
};

void test_sizes_and_offsets() {
  std::uint8_t b[64];
  CHECK_EQ(encode(b, AddOrder{}), 39u);
  CHECK_EQ(encode(b, ModifyOrder{}), 35u);
  CHECK_EQ(encode(b, DeleteOrder{}), 25u);
  CHECK_EQ(encode(b, OrderExecution{}), 42u);
  CHECK_EQ(encode(b, ReplaceOrder{}), 42u);
  CHECK_EQ(encode(b, SymbolIndexMapping{}), 44u);
  CHECK_EQ(encode(b, SymbolClear{}), 20u);

  // Hand-built Add Order bytes (spec offsets) decode as expected.
  std::uint8_t raw[39] = {};
  store_le<std::uint16_t>(raw, 39);
  store_le<std::uint16_t>(raw + 2, 100);
  store_le<std::uint32_t>(raw + 4, 123456789);
  store_le<std::uint32_t>(raw + 8, 42);
  store_le<std::uint32_t>(raw + 12, 7);
  store_le<std::uint64_t>(raw + 16, 0x1122334455667788ULL);
  store_le<std::uint32_t>(raw + 24, 1234500);
  store_le<std::uint32_t>(raw + 28, 300);
  raw[32] = 'S';
  const AddOrder a = decode_add_order(raw);
  CHECK_EQ(a.source_time_ns, 123456789u);
  CHECK_EQ(a.symbol_index, 42u);
  CHECK_EQ(a.symbol_seq_num, 7u);
  CHECK_EQ(a.order_id, 0x1122334455667788ULL);
  CHECK_EQ(a.price, 1234500u);
  CHECK_EQ(a.volume, 300u);
  CHECK(a.side == Side::Sell);
}

void test_roundtrip() {
  SymbolIndexMapping sm{};
  sm.symbol_index = 9;
  std::memcpy(sm.symbol, "IBM", 3);
  sm.price_scale_code = 4;
  sm.mpv = 100;
  sm.lot_size = 100;
  PacketBuf pb;
  pb.add(sm)
      .add(AddOrder{1, 9, 1, 1001, 1500000, 200, Side::Buy})
      .add(ModifyOrder{2, 9, 2, 1001, 1500100, 100, 1, Side::Buy})
      .add(OrderExecution{3, 9, 3, 1001, 77, 1500100, 50, 1})
      .add(ReplaceOrder{4, 9, 4, 1001, 1002, 1499900, 300, Side::Buy})
      .add(DeleteOrder{5, 9, 5, 1002})
      .add(SymbolClear{6, 7, 9, 6});
  // an unknown message type must be skipped using MsgSize
  std::uint8_t unk[10] = {};
  encode_msg_header(unk, 10, 999);
  pb.bytes.insert(pb.bytes.end(), unk, unk + 10);
  ++pb.n;
  const auto pkt = pb.finish(1);

  Collect c;
  DecodeStats st;
  CHECK(decode_messages(pkt.data(), pkt.size(), c, st));
  CHECK_EQ(st.messages, 8u);
  CHECK_EQ(c.maps.size(), 1u);
  CHECK(c.maps[0].symbol_view() == "IBM");
  CHECK_EQ(c.maps[0].mpv, 100u);
  CHECK_EQ(c.adds.size(), 1u);
  CHECK_EQ(c.adds[0].order_id, 1001u);
  CHECK_EQ(c.mods.size(), 1u);
  CHECK_EQ(c.mods[0].price, 1500100u);
  CHECK_EQ(c.mods[0].position_change, 1u);
  CHECK_EQ(c.execs.size(), 1u);
  CHECK_EQ(c.execs[0].volume, 50u);
  CHECK_EQ(c.execs[0].trade_id, 77u);
  CHECK_EQ(c.reps.size(), 1u);
  CHECK_EQ(c.reps[0].new_order_id, 1002u);
  CHECK_EQ(c.reps[0].volume, 300u);
  CHECK_EQ(c.dels.size(), 1u);
  CHECK_EQ(c.clears.size(), 1u);
  CHECK_EQ(c.clears[0].symbol_index, 9u);
  CHECK_EQ(c.others.size(), 1u);

  // truncated packet is rejected
  Collect c2;
  DecodeStats st2;
  CHECK(!decode_messages(pkt.data(), pkt.size() - 1, c2, st2));
}

void test_sequence() {
  SequenceTracker t;
  DecodeStats st;
  CHECK(t.on_packet(1, 1, 3, false, st).process);       // 1..3
  CHECK(!t.on_packet(1, 1, 3, false, st).process);      // B-line duplicate
  auto d = t.on_packet(1, 3, 3, false, st);              // 3..5, overlap of 1
  CHECK(d.process && d.skip == 1);
  CHECK(t.on_packet(1, 10, 1, false, st).process);      // gap 6..9
  CHECK_EQ(st.gaps, 1u);
  CHECK_EQ(st.missing_messages, 4u);
  CHECK_EQ(st.duplicate_packets, 1u);
  CHECK(!t.on_packet(1, 5, 0, false, st).process);      // heartbeat
  CHECK(t.on_packet(1, 1, 2, true, st).process);        // sequence reset
  CHECK(!t.on_packet(1, 1, 2, true, st).process);       // its B-line copy
  CHECK(t.on_packet(1, 3, 1, false, st).process);
  CHECK(t.on_packet(2, 500, 1, false, st).process);     // other stream is independent
}

void test_pcap_roundtrip(const std::string& dir) {
  const std::string path = dir + "/obl_test_roundtrip.pcap";
  std::vector<std::vector<std::uint8_t>> pkts;
  {
    pcap::Writer w(path);
    for (std::uint32_t i = 0; i < 5; ++i) {
      PacketBuf pb;
      pb.add(AddOrder{i, 3, i + 1, 100 + i, 1000, 100, Side::Buy});
      pkts.push_back(pb.finish(i + 1));
      w.write_udp(1000 + i, 0x0A000001, 0xE0003B01, 11001, 11001, pkts.back().data(), pkts.back().size());
      w.write_udp(1500 + i, 0x0A000002, 0xE0003C01, 11001, 11001, pkts.back().data(), pkts.back().size());
    }
  }
  const pcap::Capture cap = pcap::load(path);
  std::remove(path.c_str());
  CHECK_EQ(cap.frames, 10u);
  CHECK_EQ(cap.datagrams.size(), 10u);
  CHECK_EQ(cap.datagrams[0].dst_port, 11001);
  CHECK_EQ(cap.datagrams[0].dst_ip, 0xE0003B01u);
  CHECK_EQ(cap.datagrams[0].ts_ns, 1000u);
  CHECK_EQ(cap.datagrams[0].length, pkts[0].size());
  CHECK(std::memcmp(cap.payload(cap.datagrams[0]), pkts[0].data(), pkts[0].size()) == 0);

  // A/B arbitration keyed by port: every add exactly once
  Collect c;
  DecodeStats st;
  SequenceTracker seq;
  for (const auto& d : cap.datagrams)
    process_datagram(seq, d.stream_key(false), cap.payload(d), d.length, c, st);
  CHECK_EQ(c.adds.size(), 5u);
  CHECK_EQ(st.duplicate_packets, 5u);
}

}  // namespace

int main() {
  test_sizes_and_offsets();
  test_roundtrip();
  test_sequence();
  const char* tmp = std::getenv("TMPDIR");
  test_pcap_roundtrip(tmp ? tmp : "/tmp");
  return test_result("test_xdp");
}
