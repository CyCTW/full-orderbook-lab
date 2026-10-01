// ITCH 5.0 decoding (big-endian offsets) and the streaming file reader.

#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "obl/itch/itch.hpp"

using namespace obl;

namespace {

struct Msg {
  std::vector<std::uint8_t> b;
  explicit Msg(std::size_t n, char type) : b(n, 0) { b[0] = static_cast<std::uint8_t>(type); }
  Msg& be16(std::size_t o, std::uint16_t v) { b[o] = v >> 8; b[o + 1] = v & 0xFF; return *this; }
  Msg& be32(std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = (v >> (24 - 8 * i)) & 0xFF; return *this; }
  Msg& be64(std::size_t o, std::uint64_t v) { for (int i = 0; i < 8; ++i) b[o + i] = (v >> (56 - 8 * i)) & 0xFF; return *this; }
  Msg& ch(std::size_t o, char c) { b[o] = static_cast<std::uint8_t>(c); return *this; }
  Msg& str(std::size_t o, const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) b[o + i] = *s ? static_cast<std::uint8_t>(*s++) : ' ';
    return *this;
  }
};

void test_decode() {
  Msg r(39, 'R');
  r.be16(1, 13).str(11, "AAPL", 8).be32(21, 100);
  const auto d = itch::decode_stock_directory(r.b.data());
  CHECK_EQ(d.locate, 13);
  CHECK(d.symbol() == "AAPL");
  CHECK_EQ(d.round_lot, 100u);

  Msg a(36, 'A');
  a.be16(1, 13).be16(3, 0).be64(11, 0x0102030405060708ULL).ch(19, 'S').be32(20, 300).str(24, "AAPL", 8).be32(32, 2345600);
  // timestamp u48 @5
  a.b[5] = 0x00; a.b[6] = 0x01; a.b[7] = 0x02; a.b[8] = 0x03; a.b[9] = 0x04; a.b[10] = 0x05;
  const auto ad = itch::decode_add(a.b.data());
  CHECK_EQ(ad.locate, 13);
  CHECK_EQ(ad.ref, 0x0102030405060708ULL);
  CHECK(ad.side == Side::Sell);
  CHECK_EQ(ad.shares, 300u);
  CHECK_EQ(ad.price, 2345600u);
  CHECK_EQ(itch::timestamp(a.b.data()), 0x0001020304 * 256ULL + 0x05);

  Msg e(31, 'E');
  e.be16(1, 13).be64(11, 77).be32(19, 50);
  CHECK_EQ(itch::decode_executed(e.b.data()).shares, 50u);
  Msg x(23, 'X');
  x.be16(1, 13).be64(11, 77).be32(19, 20);
  CHECK_EQ(itch::decode_cancel(x.b.data()).shares, 20u);
  Msg u(35, 'U');
  u.be16(1, 13).be64(11, 77).be64(19, 78).be32(27, 400).be32(31, 2345700);
  const auto ud = itch::decode_replace(u.b.data());
  CHECK_EQ(ud.orig_ref, 77u);
  CHECK_EQ(ud.new_ref, 78u);
  CHECK_EQ(ud.shares, 400u);
  CHECK_EQ(ud.price, 2345700u);
  CHECK_EQ(itch::expected_size('A'), 36);
  CHECK_EQ(itch::expected_size('U'), 35);
}

void test_reader(const std::string& dir) {
#ifdef OBL_HAVE_ZLIB
  const std::string path = dir + "/obl_test_itch.gz";
  std::vector<std::vector<std::uint8_t>> msgs;
  {
    gzFile f = gzopen(path.c_str(), "wb");
    for (int i = 0; i < 300000; ++i) {  // > reader buffer, so messages straddle refills
      Msg d(19, 'D');
      d.be16(1, static_cast<std::uint16_t>(i & 0xFFFF)).be64(11, static_cast<std::uint64_t>(i));
      const std::uint8_t len[2] = {0, 19};
      gzwrite(f, len, 2);
      gzwrite(f, d.b.data(), 19);
    }
    gzclose(f);
  }
  itch::FileReader r(path);
  const std::uint8_t* m;
  std::uint16_t len;
  std::uint64_t n = 0;
  bool ok = true;
  while (r.next(m, len)) {
    ok &= len == 19 && m[0] == 'D' && itch::decode_delete(m).ref == n;
    ++n;
  }
  std::remove(path.c_str());
  CHECK(ok);
  CHECK_EQ(n, 300000u);
  CHECK_EQ(r.truncated(), 0u);
#endif
}

}  // namespace

int main() {
  test_decode();
  const char* tmp = std::getenv("TMPDIR");
  test_reader(tmp ? tmp : "/tmp");
  return test_result("test_itch");
}
