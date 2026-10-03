// Random corruption of valid OCG-C messages fed to every decoder and to the session. The checksum
// is recomputed after mutating so the damage gets past the CRC and reaches the parsers. Run under
// ASan / UBSan: the property is "no crash, no out-of-bounds read, no undefined behaviour".

#include <cstdint>
#include <random>
#include <vector>

#include "check.hpp"
#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/order_template.hpp"
#include "obl/gw/ocgc/session.hpp"

using namespace obl;
using namespace obl::gw::ocgc;

namespace {

struct NullTx {
  void send(const std::uint8_t*, std::size_t) {}
  void close() {}
};
struct NullApp {
  std::uint64_t n = 0;
  void on_session_active(const SessionFields&) {}
  void on_business(const Header& h, const std::uint8_t* m, std::size_t len) {
    ExecReport er;
    n += decode_exec_report(m, len, er);
    RejectInfo ri;
    n += decode_reject(m, len, h.type, ri);
    MassCancelReport mc;
    n += decode_mass_cancel_report(m, len, mc);
  }
  void on_not_sent(std::uint32_t, const std::uint8_t*, std::size_t) {}
  void on_session_closed(CloseReason) {}
};

std::vector<std::vector<std::uint8_t>> corpus() {
  std::vector<std::vector<std::uint8_t>> c;
  std::uint8_t b[1024];
  auto add = [&](std::size_t n) { c.emplace_back(b, b + n); };
  ExecReport er;
  namespace x = exec_report;
  er.present = 1ull << x::ClOrdId | 1ull << x::SecurityId | 1ull << x::Side | 1ull << x::OrderId | 1ull << x::ExecId |
               1ull << x::OrdStatus | 1ull << x::ExecType | 1ull << x::CumQty | 1ull << x::LeavesQty |
               1ull << x::Reason | 1ull << x::Text | 1ull << x::TradeMatchId | 1ull << x::ExecQty;
  er.cl_ord_id = "10000001";
  er.security_id = "700";
  er.order_id = "1";
  er.exec_id = "2";
  er.reason = "why";
  er.text = "txt";
  er.trade_match_id = "t";
  er.exec_type = ExecType::Trade;
  add(encode_exec_report(b, 2, "OCGC", er));
  add(encode_logon_reply(b, 1, "OCGC", 2));
  add(encode_logout(b, 3, "OCGC", "bye"));
  add(encode_heartbeat(b, 4, "OCGC", 9));
  add(encode_test_request(b, 5, "OCGC", 7));
  add(encode_resend_request(b, 6, "OCGC", 1, 0));
  add(encode_sequence_reset(b, 7, "OCGC", 9, true, true));
  const OrderContext ctx{"C", "1234", 20261003};
  add(encode_cancel(b, 8, ctx, "700", WireSide::Buy, 10000002, 10000001, 0));
  add(encode_amend(b, 9, ctx, "700", WireSide::Buy, 10000003, 10000001, 1, 1, Tif::Day, 0));
  add(encode_mass_cancel(b, 10, ctx, 10000004, mass_cancel::kAllOrders, {}, 0, 0));
  MassCancelReport m;
  m.report_id = "r";
  m.reason = "x";
  add(encode_mass_cancel_report(b, 11, "OCGC", m, "20261003-00:00:00.000000"));
  Writer w(b, MsgType::BusinessMessageReject, 12, "OCGC");
  w.u16(business_reject::BusinessRejectCode, 1).var_alnum(business_reject::Reason, "r");
  add(w.finish());
  NewOrderTemplate t({"C", "1234", "700", "B", WireSide::Buy, 20261003});
  t.fill({13, 10000005, 0, 1, 1});
  c.emplace_back(t.data(), t.data() + t.size());
  return c;
}

void test_fuzz_decoders() {
  const auto base = corpus();
  std::mt19937_64 rng(42);
  std::uint64_t parsed = 0;
  for (int iter = 0; iter < 300000; ++iter) {
    std::vector<std::uint8_t> m = base[rng() % base.size()];
    const int edits = 1 + static_cast<int>(rng() % 6);
    for (int e = 0; e < edits; ++e) {
      const std::size_t i = rng() % m.size();
      switch (rng() % 4) {
        case 0: m[i] = static_cast<std::uint8_t>(rng()); break;  // any byte, incl. presence map / lengths
        case 1: m[i] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
        case 2: if (m.size() > hdr::kSize + 8) m.resize(m.size() - 1 - rng() % 8); break;  // truncate
        case 3: m.insert(m.begin() + static_cast<std::ptrdiff_t>(i), static_cast<std::uint8_t>(rng())); break;
      }
    }
    if (m.size() < hdr::kSize + kTrailerSize) continue;
    // keep the frame consistent half of the time so the body parsers run
    if (rng() % 2) {
      store_le<std::uint16_t>(m.data() + hdr::kLength, static_cast<std::uint16_t>(m.size()));
      store_le<std::uint32_t>(m.data() + m.size() - 4, checksum(m.data(), m.size() - 4));
    }
    if (!valid_frame(m.data(), m.size())) continue;
    const Header h = read_header(m.data());
    ExecReport er;
    parsed += decode_exec_report(m.data(), m.size(), er);
    if (er.has(exec_report::ClOrdId)) parse_cl_ord_id(er.cl_ord_id);
    (void)er.reason.view();
    SessionFields sf;
    parsed += decode_session(m.data(), m.size(), h.type, sf);
    OrderRequest rq;
    parsed += decode_request(m.data(), m.size(), h.type, rq);
    RejectInfo ri;
    parsed += decode_reject(m.data(), m.size(), h.type, ri);
    MassCancelReport mc;
    parsed += decode_mass_cancel_report(m.data(), m.size(), mc);
    LookupResult lr;
    parsed += decode_lookup_response(m.data(), m.size(), lr);
  }
  CHECK(parsed > 0);
}

void test_fuzz_session() {
  const auto base = corpus();
  std::mt19937_64 rng(7);
  NullTx tx;
  NullApp app;
  SessionConfig cfg;
  cfg.comp_id = "C";
  cfg.store_reserve_bytes = 1 << 20;
  cfg.store_reserve_msgs = 1 << 14;
  for (int round = 0; round < 2000; ++round) {
    Session<NullTx, NullApp> s(cfg, tx, app);
    s.on_connected(0);
    std::uint8_t b[256];
    const std::size_t n = encode_logon_reply(b, 1, "OCGC", 2);
    s.on_bytes(b, n, 0);
    // a stream of valid and corrupted messages with random sequence numbers, in random chunks
    std::vector<std::uint8_t> stream;
    for (int k = 0; k < 20; ++k) {
      std::vector<std::uint8_t> m = base[rng() % base.size()];
      store_le<std::uint32_t>(m.data() + hdr::kSeqNum, static_cast<std::uint32_t>(rng() % 30));
      if (rng() % 4 == 0) m[rng() % m.size()] = static_cast<std::uint8_t>(rng());
      store_le<std::uint32_t>(m.data() + m.size() - 4, checksum(m.data(), m.size() - 4));
      stream.insert(stream.end(), m.begin(), m.end());
    }
    for (std::size_t off = 0; off < stream.size();) {
      const std::size_t chunk = std::min<std::size_t>(stream.size() - off, 1 + rng() % 400);
      s.on_bytes(stream.data() + off, chunk, off);
      s.on_timer(off * 1'000'000'000ull);
      off += chunk;
    }
  }
  CHECK(true);
}

}  // namespace

int main() {
  test_fuzz_decoders();
  test_fuzz_session();
  return test_result("test_fuzz");
}
