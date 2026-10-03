// OCG-C binary protocol: CRC32C variants, message encoding, template patching, incremental CRC.

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/order_template.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

void test_crc_known_vectors() {
  // CRC-32C check value (RFC 3720 / iSCSI).
  const char* s = "123456789";
  CHECK_EQ(crc32c_bitwise(0, s, 9), 0xE3069283u);
  CHECK_EQ(crc32c_sarwate(0, s, 9), 0xE3069283u);
  CHECK_EQ(crc32c_slice8(0, s, 9), 0xE3069283u);
#if defined(__SSE4_2__)
  CHECK_EQ(crc32c_hw(0, s, 9), 0xE3069283u);
#endif
  // RFC 3720 B.4: 32 bytes of zeros.
  std::uint8_t z[32] = {};
  CHECK_EQ(crc32c_bitwise(0, z, 32), 0x8A9136AAu);
}

void test_crc_variants_agree() {
  std::mt19937_64 rng(1);
  std::vector<std::uint8_t> buf(1024);
  for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
  for (std::size_t n = 0; n <= 300; ++n) {
    for (std::size_t off : {0, 1, 3, 7}) {
      const std::uint32_t ref = crc32c_bitwise(0, buf.data() + off, n);
      CHECK_EQ(crc32c_sarwate(0, buf.data() + off, n), ref);
      CHECK_EQ(crc32c_slice8(0, buf.data() + off, n), ref);
#if defined(__SSE4_2__)
      CHECK_EQ(crc32c_hw(0, buf.data() + off, n), ref);
#endif
    }
  }
  // chaining
  const std::uint32_t a = crc32c_slice8(0, buf.data(), 100);
  CHECK_EQ(crc32c_slice8(a, buf.data() + 100, 77), crc32c_bitwise(0, buf.data(), 177));
}

void test_crc_shift() {
  std::mt19937 rng(2);
  for (int i = 0; i < 2000; ++i) {
    const std::uint32_t c = rng();
    const std::size_t zeros = rng() % 300;
    // reference: feed zero bytes through the raw register one at a time
    std::uint32_t ref = c;
    for (std::size_t k = 0; k < zeros; ++k)
      for (int j = 0; j < 8; ++j) ref = (ref >> 1) ^ (kCrc32cPoly & (0u - (ref & 1u)));
    CHECK_EQ(crc32c_shift_slow(c, zeros), ref);
#if defined(__SSE4_2__) && defined(__PCLMUL__)
    CHECK_EQ(Crc32cShift(zeros)(c), ref);
#endif
  }
}

const NewOrderStatic kStatic{
    .comp_id = "CO99999901",
    .submitting_broker_id = "1234",
    .security_id = "700",
    .bcan = "ABC123.2568",
    .side = WireSide::Buy,
    .trade_date = 20261003,
};

void test_new_order_layout() {
  std::uint8_t buf[256];
  const NewOrderVar v{.seq = 42, .cl_ord_id = 12345678, .us_of_day = 3723456789ull,  // 01:02:03.456789
                      .price = 512'500'000'000, .qty = 100 * kDecimalScale};
  const std::size_t n = encode_new_order(buf, kStatic, v);
  CHECK_EQ(n, 184u);
  CHECK_EQ(buf[0], kStx);
  CHECK_EQ(load_le<std::uint16_t>(buf + 1), 184);
  CHECK_EQ(buf[3], 11);
  CHECK_EQ(load_le<std::uint32_t>(buf + 4), 42u);
  CHECK(read_alnum(buf + 10, 12) == "CO99999901");
  // presence map: bits 0-4, 6-10, 18, 22 -> 0xFB 0xE0 0x22 (MSB first)
  CHECK_EQ(buf[22], 0xFB);
  CHECK_EQ(buf[23], 0xE0);
  CHECK_EQ(buf[24], 0x22);
  for (int i = 25; i < 54; ++i) CHECK_EQ(buf[i], 0);
  CHECK(read_alnum(buf + 54, 21) == "12345678");
  CHECK(read_alnum(buf + 75, 12) == "1234");
  CHECK(read_alnum(buf + 87, 21) == "700");
  CHECK_EQ(buf[108], 8);
  CHECK(read_alnum(buf + 109, 5) == "XHKG");
  CHECK(read_alnum(buf + 114, 25) == "20261003-01:02:03.456789");
  CHECK_EQ(buf[139], 1);
  CHECK_EQ(buf[140], 2);
  CHECK_EQ(load_le<std::int64_t>(buf + 141), 512'500'000'000);
  CHECK_EQ(load_le<std::int64_t>(buf + 149), 100 * kDecimalScale);
  CHECK_EQ(load_le<std::uint16_t>(buf + 157), 1);
  CHECK(read_alnum(buf + 159, 21) == "ABC123.2568");
  CHECK(verify_checksum(buf, n));
  CHECK_EQ(load_le<std::uint32_t>(buf + 180), crc32c_bitwise(0, buf, 180));

  // decode round trip
  const Header h = read_header(buf);
  CHECK(h.type == MsgType::NewOrder);
  CHECK_EQ(h.length, 184);
  CHECK_EQ(h.seq, 42u);
  int fields = 0;
  std::string time;
  std::int64_t price = 0;
  const bool ok = for_each_field(buf, n, new_order::kFields, [&](const FieldRef& f) {
    ++fields;
    if (f.bit == new_order::TransactTime) time = std::string(read_alnum(f.data, f.size));
    if (f.bit == new_order::Price) price = load_le<std::int64_t>(f.data);
  });
  CHECK(ok);
  CHECK_EQ(fields, 12);
  CHECK(time == "20261003-01:02:03.456789");
  CHECK_EQ(price, 512'500'000'000);

  // corruption is detected
  buf[100] ^= 1;
  CHECK(!verify_checksum(buf, n));
  CHECK(!for_each_field(buf, n, new_order::kFields, [](const FieldRef&) {}));
}

void test_optional_and_variable_fields() {
  std::uint8_t buf[256];
  Writer w(buf, MsgType::CancelOrder, 7, "COMP");
  w.alnum(cancel_order::ClOrdId, "10000001", field_size::kClOrdId)
      .alnum(cancel_order::SubmittingBrokerId, "1234", field_size::kBrokerId)
      .alnum(cancel_order::SecurityId, "5", field_size::kSecurityId)
      .u8(cancel_order::SecurityIdSource, 8)
      .alnum(cancel_order::TransactTime, "20261003-01:02:03.000000", field_size::kTransactTime)
      .u8(cancel_order::Side, 2)
      .alnum(cancel_order::OrigClOrdId, "10000000", field_size::kClOrdId)
      .var_alnum(cancel_order::Text, "kill switch");
  const std::size_t n = w.finish();
  CHECK_EQ(n, 54u + 21 + 12 + 21 + 1 + 25 + 1 + 21 + (2 + 12) + 4);
  std::string text, orig;
  const bool ok = for_each_field(buf, n, cancel_order::kFields, [&](const FieldRef& f) {
    if (f.bit == cancel_order::Text) text = std::string(read_alnum(f.data, f.size));
    if (f.bit == cancel_order::OrigClOrdId) orig = std::string(read_alnum(f.data, f.size));
  });
  CHECK(ok);
  CHECK(text == "kill switch");
  CHECK(orig == "10000000");
}

void test_template_matches_reference() {
  NewOrderTemplate tpl(kStatic);
  CHECK_EQ(tpl.size(), 184u);
  std::mt19937_64 rng(3);
  std::uint8_t ref[256];
  for (int i = 0; i < 20000; ++i) {
    const NewOrderVar v{
        .seq = static_cast<std::uint32_t>(rng()),
        .cl_ord_id = static_cast<std::uint32_t>(NewOrderTemplate::kMinClOrdId + rng() % 90'000'000),
        .us_of_day = rng() % (86'400ull * 1'000'000),
        .price = static_cast<std::int64_t>(rng()),
        .qty = static_cast<std::int64_t>(rng() % 1'000'000) * kDecimalScale,
    };
    const std::size_t n = encode_new_order(ref, kStatic, v);
#if defined(__SSE4_2__)
    tpl.fill(v);
    CHECK_EQ(n, tpl.size());
    CHECK(std::memcmp(ref, tpl.data(), n) == 0);
#endif
#if defined(__SSE4_2__) && defined(__PCLMUL__)
    std::memset(tpl.data() + n - 4, 0xAB, 4);
    tpl.fill_incremental(v);
    CHECK(std::memcmp(ref, tpl.data(), n) == 0);
    std::memset(tpl.data() + 54, 0xCD, n - 54);  // scribble: fill_regs must rewrite every span
    tpl = NewOrderTemplate(kStatic);
    tpl.fill_regs(v);
    CHECK(std::memcmp(ref, tpl.data(), n) == 0);
#endif
  }
  // non-default TIF adds one byte
  NewOrderStatic ioc = kStatic;
  ioc.tif = Tif::IOC;
  NewOrderTemplate t2(ioc);
  CHECK_EQ(t2.size(), 185u);
#if defined(__SSE4_2__) && defined(__PCLMUL__)
  const NewOrderVar v{1, 10000000, 0, 1, 1};
  t2.fill_incremental(v);
  CHECK(verify_checksum(t2.data(), t2.size()));
  CHECK_EQ(encode_new_order(ref, ioc, v), 185u);
  CHECK(std::memcmp(ref, t2.data(), 185) == 0);
#endif
}

void test_exec_report_roundtrip() {
  namespace b = exec_report;
  ExecReport er;
  er.present = 1ull << b::ClOrdId | 1ull << b::SubmittingBrokerId | 1ull << b::SecurityId | 1ull << b::SecurityIdSource |
               1ull << b::SecurityExchange | 1ull << b::TransactTime | 1ull << b::Side | 1ull << b::OrderId |
               1ull << b::OrdType | 1ull << b::Price | 1ull << b::OrderQty | 1ull << b::Reason | 1ull << b::ExecId |
               1ull << b::OrdStatus | 1ull << b::ExecType | 1ull << b::CumQty | 1ull << b::LeavesQty |
               1ull << b::ExecQty | 1ull << b::ExecPrice | 1ull << b::TradeMatchId | 1ull << b::AggressorIndicator;
  er.cl_ord_id = "12345678";
  er.submitting_broker_id = "1234";
  er.security_id = "700";
  er.transact_time = "20261003-01:02:03.456789";
  er.side = 2;
  er.order_id = "77777";
  er.ord_type = 2;
  er.price = 400 * kDecimalScale;
  er.order_qty = 300 * kDecimalScale;
  er.reason = "partial";
  er.exec_id = "EX1";
  er.ord_status = OrdStatus::PartiallyFilled;
  er.exec_type = ExecType::Trade;
  er.cum_qty = 100 * kDecimalScale;
  er.leaves_qty = 200 * kDecimalScale;
  er.exec_qty = 100 * kDecimalScale;
  er.exec_price = 400 * kDecimalScale;
  er.trade_match_id = "TM9";
  er.aggressor = 1;
  std::uint8_t buf[512];
  const std::size_t n = encode_exec_report(buf, 9, "OCGC", er);
  CHECK(valid_frame(buf, n));
  ExecReport d;
  CHECK(decode_exec_report(buf, n, d));
  CHECK_EQ(d.present, er.present);
  CHECK(d.cl_ord_id == "12345678" && d.order_id == "77777" && d.exec_id == "EX1" && d.reason == "partial");
  CHECK(d.trade_match_id == "TM9" && d.transact_time == er.transact_time && d.submitting_broker_id == "1234");
  CHECK(d.exec_type == ExecType::Trade && d.ord_status == OrdStatus::PartiallyFilled);
  CHECK_EQ(d.price, er.price);
  CHECK_EQ(d.exec_qty, er.exec_qty);
  CHECK_EQ(d.leaves_qty, er.leaves_qty);
  CHECK_EQ(d.aggressor, 1);
  CHECK(d.has(b::TradeMatchId) && !d.has(b::OrigClOrdId));
  CHECK_EQ(parse_cl_ord_id(d.cl_ord_id), 12345678u);
  CHECK_EQ(parse_cl_ord_id("0123"), 0u);
  CHECK_EQ(parse_cl_ord_id("123456789"), 0u);

  // unknown presence bit (37 is not defined for Execution Report) -> rejected, not misparsed
  buf[hdr::kPresenceMap + 37 / 8] |= 0x80 >> (37 % 8);
  store_le<std::uint32_t>(buf + n - 4, checksum(buf, n - 4));
  CHECK(!decode_exec_report(buf, n, d));

  // Business Message Reject
  Writer w(buf, MsgType::BusinessMessageReject, 3, "OCGC");
  w.u16(business_reject::BusinessRejectCode, 2)
      .var_alnum(business_reject::Reason, "throttle: 120 ms left")
      .u8(business_reject::RefMsgType, 11)
      .u32(business_reject::RefSeqNum, 41)
      .alnum(business_reject::BusinessRejectRefId, "10000005", field_size::kClOrdId);
  const std::size_t m = w.finish();
  RejectInfo r;
  CHECK(valid_frame(buf, m));
  CHECK(decode_reject(buf, m, MsgType::BusinessMessageReject, r));
  CHECK_EQ(r.code, 2);
  CHECK(r.reason == "throttle: 120 ms left");
  CHECK_EQ(r.ref_msg_type, 11);
  CHECK_EQ(r.ref_seq, 41u);
  CHECK(r.ref_id == "10000005");
}

void test_presence_map_high_bits() {
  // fields at bits 63, 64 and 200 cross the 64-bit words of the presence map
  FieldTable t{};
  t[63] = {FieldKind::UInt8, 1, "a"};
  t[64] = {FieldKind::UInt32, 4, "b"};
  t[200] = {FieldKind::Decimal, 8, "c"};
  std::uint8_t buf[256];
  Writer w(buf, MsgType::NewOrder, 1, "X");
  w.u8(63, 7).u32(64, 0xDEADBEEF).decimal(200, -5);
  const std::size_t n = w.finish();
  std::vector<unsigned> bits;
  CHECK(for_each_field(buf, n, t, [&](const FieldRef& f) {
    bits.push_back(f.bit);
    if (f.bit == 63) CHECK_EQ(as_u8(f), 7);
    if (f.bit == 64) CHECK_EQ(as_u32(f), 0xDEADBEEFu);
    if (f.bit == 200) CHECK_EQ(as_decimal(f), -5);
  }));
  CHECK((bits == std::vector<unsigned>{63, 64, 200}));
}

void test_requests_roundtrip() {
  const OrderContext ctx{"CO99999901", "1234", 20261003};
  std::uint8_t buf[512];
  OrderRequest r;

  std::size_t n = encode_cancel(buf, 5, ctx, "700", WireSide::Sell, 10000009, 10000001, 3600ull * 1'000'000);
  CHECK(valid_frame(buf, n));
  CHECK(decode_request(buf, n, MsgType::CancelOrder, r));
  CHECK_EQ(r.cl_ord_id, 10000009u);
  CHECK_EQ(r.orig_cl_ord_id, 10000001u);
  CHECK(r.security_id == "700");
  CHECK_EQ(r.side, 2);

  n = encode_amend(buf, 6, ctx, "5", WireSide::Buy, 10000010, 10000002, 51 * kDecimalScale, 400 * kDecimalScale,
                   Tif::Day, 0);
  CHECK(valid_frame(buf, n));
  CHECK(decode_request(buf, n, MsgType::AmendOrder, r));
  CHECK_EQ(r.cl_ord_id, 10000010u);
  CHECK_EQ(r.orig_cl_ord_id, 10000002u);
  CHECK_EQ(r.price, 51 * kDecimalScale);
  CHECK_EQ(r.qty, 400 * kDecimalScale);
  CHECK_EQ(r.ord_type, 2);
  CHECK(r.has_price);

  n = encode_mass_cancel(buf, 7, ctx, 10000011, mass_cancel::kAllOrders, {}, 0, 0);
  CHECK(decode_request(buf, n, MsgType::MassCancel, r));
  CHECK_EQ(r.mass_type, mass_cancel::kAllOrders);
  CHECK_EQ(r.side, 0);
  n = encode_mass_cancel(buf, 8, ctx, 10000012, mass_cancel::kForSecurity, "700", 1, 0);
  CHECK(decode_request(buf, n, MsgType::MassCancel, r));
  CHECK(r.security_id == "700");
  CHECK_EQ(r.side, 1);

  // the New Order template decodes the same way
  NewOrderTemplate tpl(kStatic);
  tpl.fill({1, 10000013, 0, 400 * kDecimalScale, 100 * kDecimalScale});
  CHECK(decode_request(tpl.data(), tpl.size(), MsgType::NewOrder, r));
  CHECK_EQ(r.cl_ord_id, 10000013u);
  CHECK_EQ(r.qty, 100 * kDecimalScale);
  CHECK_EQ(r.side, 1);

  MassCancelReport m;
  m.cl_ord_id = 10000011;
  m.request_type = mass_cancel::kAllOrders;
  m.response = 0;
  m.reject_code = 99;
  m.report_id = "MA1";
  m.reason = "no orders";
  n = encode_mass_cancel_report(buf, 3, "OCGC", m, "20261003-01:00:00.000000");
  MassCancelReport d;
  CHECK(valid_frame(buf, n));
  CHECK(decode_mass_cancel_report(buf, n, d));
  CHECK_EQ(d.cl_ord_id, 10000011u);
  CHECK_EQ(d.response, 0);
  CHECK_EQ(d.reject_code, 99);
  CHECK(d.report_id == "MA1" && d.reason == "no orders");

  n = encode_lookup_request(buf, "CO99999901");
  CHECK(valid_frame(buf, n));
  CHECK_EQ(read_header(buf).seq, 1u);
  LookupResult lr;
  lr.accepted = true;
  lr.primary_ip = "10.1.2.3";
  lr.primary_port = 5001;
  lr.secondary_ip = "10.1.2.4";
  lr.secondary_port = 5002;
  n = encode_lookup_response(buf, "OCGC", lr);
  LookupResult ld;
  CHECK(decode_lookup_response(buf, n, ld));
  CHECK(ld.accepted && ld.primary_ip == "10.1.2.3" && ld.primary_port == 5001 && ld.secondary_port == 5002);
}

}  // namespace

int main() {
  test_crc_known_vectors();
  test_crc_variants_agree();
  test_crc_shift();
  test_new_order_layout();
  test_optional_and_variable_fields();
  test_template_matches_reference();
  test_exec_report_roundtrip();
  test_presence_map_high_bits();
  test_requests_roundtrip();
  return test_result("test_ocgc");
}
