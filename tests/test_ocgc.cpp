// OCG-C binary protocol: CRC32C variants, message encoding, template patching, incremental CRC.

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
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

}  // namespace

int main() {
  test_crc_known_vectors();
  test_crc_variants_agree();
  test_crc_shift();
  test_new_order_layout();
  test_optional_and_variable_fields();
  test_template_matches_reference();
  return test_result("test_ocgc");
}
