// OCG-C New Order encoding and CRC32C checksum cost.
//
// Two measurements per variant:
//   chained   mean ns/op where each op's input depends on the previous op's checksum, so ops
//             cannot overlap: the latency the send path actually pays
//   per call  rdtsc around every op (fenced), timer overhead subtracted: p50 / p99 / p99.9
//
// usage: obl_ocgc_bench [--iters N]

#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/order_template.hpp"
#include "obl/gw/ocgc/session.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

double tsc_ghz() {
  static const double ghz = [] {
    using Clock = std::chrono::steady_clock;
    const auto w0 = Clock::now();
    const auto c0 = __rdtsc();
    while (Clock::now() - w0 < std::chrono::milliseconds(200)) {
    }
    const auto c1 = __rdtsc();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - w0).count();
    return static_cast<double>(c1 - c0) / ns;
  }();
  return ghz;
}

inline void escape(const void* p) { asm volatile("" : : "r"(p) : "memory"); }

struct Stats {
  double chained_ns, p50, p99, p999;
};

std::size_t g_iters = 2'000'000;

// Lambdas rather than function pointers so every variant is inlined into its loop.
constexpr auto kBitwise = [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_bitwise(c, p, n); };
constexpr auto kSarwate = [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_sarwate(c, p, n); };
constexpr auto kSlice8 = [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_slice8(c, p, n); };
#if defined(__SSE4_2__)
constexpr auto kHw = [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_hw(c, p, n); };
#endif

// op(i, prev) -> uint32 checksum-like value that the next call depends on.
template <class Op>
Stats measure(Op&& op, std::size_t iters = 0) {
  const std::size_t saved = g_iters;
  if (iters) g_iters = iters;
  struct Restore {
    std::size_t& r;
    std::size_t v;
    ~Restore() { r = v; }
  } restore{g_iters, saved};
  // warm up
  std::uint32_t prev = 0;
  for (std::size_t i = 0; i < 20000; ++i) prev = op(i, prev);

  const auto c0 = __rdtsc();
  for (std::size_t i = 0; i < g_iters; ++i) prev = op(i, prev);
  const auto c1 = __rdtsc();
  const double chained = static_cast<double>(c1 - c0) / static_cast<double>(g_iters) / tsc_ghz();

  // per call
  const std::size_t n = std::min<std::size_t>(g_iters, 1'000'000);
  std::vector<std::uint32_t> lat(n), empty(n);
  unsigned aux;
  for (std::size_t i = 0; i < n; ++i) {
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    const auto t1 = __rdtscp(&aux);
    _mm_lfence();
    empty[i] = static_cast<std::uint32_t>(t1 - t0);
  }
  for (std::size_t i = 0; i < n; ++i) {
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    prev = op(i, prev);
    const auto t1 = __rdtscp(&aux);
    _mm_lfence();
    lat[i] = static_cast<std::uint32_t>(t1 - t0);
  }
  escape(&prev);
  std::sort(empty.begin(), empty.end());
  std::sort(lat.begin(), lat.end());
  const double overhead = empty[n / 2];
  auto pct = [&](double q) {
    const double c = static_cast<double>(lat[static_cast<std::size_t>(q * (n - 1))]) - overhead;
    return std::max(0.0, c) / tsc_ghz();
  };
  return {chained, pct(0.50), pct(0.99), pct(0.999)};
}

void print_row(const char* name, const Stats& s) {
  std::printf("  %-34s %8.1f %8.1f %8.1f %8.1f\n", name, s.chained_ns, s.p50, s.p99, s.p999);
}

void print_header(const char* title) {
  std::printf("\n%s\n  %-34s %8s %8s %8s %8s\n", title, "variant", "chained", "p50", "p99", "p99.9");
}

const NewOrderStatic kStatic{
    .comp_id = "CO99999901",
    .submitting_broker_id = "1234",
    .security_id = "700",
    .bcan = "ABC123.2568",
    .side = WireSide::Buy,
    .trade_date = 20261003,
};

std::vector<NewOrderVar> make_inputs() {
  std::mt19937_64 rng(7);
  std::vector<NewOrderVar> v(4096);
  std::uint64_t t = 9ull * 3600 * 1'000'000 + 30ull * 60 * 1'000'000;  // 09:30 HKT-ish, UTC irrelevant
  for (std::size_t i = 0; i < v.size(); ++i) {
    t += 1 + rng() % 5000;
    v[i] = NewOrderVar{
        .seq = static_cast<std::uint32_t>(i + 1),
        .cl_ord_id = static_cast<std::uint32_t>(NewOrderTemplate::kMinClOrdId + i),
        .us_of_day = t,
        .price = static_cast<std::int64_t>(400'000 + rng() % 2000) * 100'000,  // 400.000 .. 401.999
        .qty = static_cast<std::int64_t>(100 * (1 + rng() % 50)) * kDecimalScale,
    };
  }
  return v;
}

// From-scratch encoding with snprintf for the ASCII fields: what a straightforward first version
// tends to look like.
std::size_t encode_new_order_snprintf(std::uint8_t* buf, const NewOrderStatic& s, const NewOrderVar& v) {
  namespace b = new_order;
  namespace fs = field_size;
  Writer w(buf, MsgType::NewOrder, v.seq, s.comp_id);
  char* id = w.alnum_slot(b::ClOrdId, fs::kClOrdId);
  std::snprintf(id, fs::kClOrdId, "%u", v.cl_ord_id);
  w.alnum(b::SubmittingBrokerId, s.submitting_broker_id, fs::kBrokerId);
  w.alnum(b::SecurityId, s.security_id, fs::kSecurityId);
  w.u8(b::SecurityIdSource, kSecurityIdSourceExchangeSymbol);
  w.alnum(b::SecurityExchange, "XHKG", fs::kSecurityExchange);
  char* t = w.alnum_slot(b::TransactTime, fs::kTransactTime);
  const auto secs = static_cast<unsigned>(v.us_of_day / 1'000'000);
  char tmp[64];
  std::snprintf(tmp, sizeof tmp, "%08u-%02u:%02u:%02u.%06u", s.trade_date, secs / 3600, secs / 60 % 60,
                secs % 60, static_cast<unsigned>(v.us_of_day % 1'000'000));
  std::memcpy(t, tmp, fs::kTransactTime - 1);
  w.u8(b::Side, static_cast<std::uint8_t>(s.side));
  w.u8(b::OrdType, static_cast<std::uint8_t>(OrdType::Limit));
  w.decimal(b::Price, v.price);
  w.decimal(b::OrderQty, v.qty);
  w.u16(b::DisclosureInstructions, kDisclosureNone);
  w.alnum(b::SubmittingBcan, s.bcan, fs::kBcan);
  return w.finish();
}

// Field walk testing all 256 presence bits one by one (the first version of for_each_field).
template <class F>
bool for_each_field_naive(const std::uint8_t* msg, std::size_t len, const FieldTable& defs, F&& f) {
  const std::uint8_t* p = msg + hdr::kSize;
  const std::uint8_t* end = msg + len - kTrailerSize;
  for (unsigned bit = 0; bit < 256; ++bit) {
    if (!presence_bit(msg, bit)) continue;
    const FieldDef& d = defs[bit];
    std::size_t size = d.size;
    const std::uint8_t* data = p;
    if (d.kind == FieldKind::VarAlnum) {
      size = load_le<std::uint16_t>(p);
      data = p + 2;
      p = data + size;
    } else {
      if (d.kind == FieldKind::Unused || static_cast<std::size_t>(end - p) < size) return false;
      p += size;
    }
    f(FieldRef{bit, &d, data, size});
  }
  return p == end;
}

// A partial fill as OCG-C sends it (§7.6.7.10), mandatory fields plus a few optional ones.
std::size_t make_trade_report(std::uint8_t* buf, std::uint32_t seq) {
  namespace b = exec_report;
  ExecReport er;
  er.present = 1ull << b::ClOrdId | 1ull << b::SubmittingBrokerId | 1ull << b::SecurityId | 1ull << b::SecurityIdSource |
               1ull << b::SecurityExchange | 1ull << b::TransactTime | 1ull << b::Side | 1ull << b::OrderId |
               1ull << b::OrdType | 1ull << b::Price | 1ull << b::OrderQty | 1ull << b::ExecId | 1ull << b::OrdStatus |
               1ull << b::ExecType | 1ull << b::CumQty | 1ull << b::LeavesQty | 1ull << b::MatchType |
               1ull << b::ExecQty | 1ull << b::ExecPrice | 1ull << b::TradeMatchId | 1ull << b::AggressorIndicator;
  er.cl_ord_id = "10000042";
  er.submitting_broker_id = "1234";
  er.security_id = "700";
  er.transact_time = "20261003-01:30:00.123456";
  er.side = 1;
  er.order_id = "5550001";
  er.ord_type = 2;
  er.price = 400 * kDecimalScale;
  er.order_qty = 300 * kDecimalScale;
  er.exec_id = "88000001";
  er.ord_status = OrdStatus::PartiallyFilled;
  er.exec_type = ExecType::Trade;
  er.cum_qty = 100 * kDecimalScale;
  er.leaves_qty = 200 * kDecimalScale;
  er.match_type = 4;
  er.exec_qty = 100 * kDecimalScale;
  er.exec_price = 400 * kDecimalScale;
  er.trade_match_id = "T123456";
  er.aggressor = 1;
  return encode_exec_report(buf, seq, "OCGC", er);
}

struct NullTransport {
  std::uint64_t bytes = 0;
  void send(const std::uint8_t*, std::size_t n) { bytes += n; }
  void close() {}
};

struct BenchApp {
  std::uint64_t sum = 0;
  void on_session_active(const SessionFields&) {}
  void on_business(const Header&, const std::uint8_t* m, std::size_t n) {
    ExecReport er;
    if (decode_exec_report(m, n, er)) sum += static_cast<std::uint64_t>(er.leaves_qty) + parse_cl_ord_id(er.cl_ord_id);
  }
  void on_not_sent(std::uint32_t, const std::uint8_t*, std::size_t) {}
  void on_session_closed(CloseReason) {}
};

using BenchSession = Session<NullTransport, BenchApp>;

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--iters" && i + 1 < argc) g_iters = std::strtoull(argv[++i], nullptr, 10);
  }
#if !defined(__SSE4_2__) || !defined(__PCLMUL__)
  std::fprintf(stderr, "build with -march=native on an x86 CPU with SSE4.2 and PCLMUL\n");
  return 1;
#else
  const auto inputs = make_inputs();
  const std::size_t mask = inputs.size() - 1;
  NewOrderTemplate tpl(kStatic);
  std::printf("OCG-C New Order (limit, Day, mandatory fields): %zu bytes, checksum over %zu bytes\n",
              tpl.size(), tpl.size() - kTrailerSize);
  std::printf("tsc %.3f GHz, %zu iterations; ns per message\n", tsc_ghz(), g_iters);

  // --- checksum only, over a 180-byte message ------------------------------------------------
  {
    alignas(64) std::uint8_t buf[256];
    tpl.fill(inputs[0]);
    std::memcpy(buf, tpl.data(), tpl.size());
    const std::size_t n = tpl.size() - kTrailerSize;
    print_header("CRC32C over the message (header + body)");
    auto crc_op = [&](auto fn) {
      return [&, fn](std::size_t, std::uint32_t prev) {
        buf[4] = static_cast<std::uint8_t>(prev);  // next input depends on previous result
        return fn(0, buf, n);
      };
    };
    print_row("bitwise", measure(crc_op(kBitwise)));
    print_row("sarwate (1 table, 1 B/step)", measure(crc_op(kSarwate)));
    print_row("slice-by-8 (8 tables, 8 B/step)", measure(crc_op(kSlice8)));
    print_row("sse4.2 crc32 (8 B/instr)", measure(crc_op(kHw)));
    print_row("incremental (4 spans + pclmul)", measure([&](std::size_t, std::uint32_t prev) {
                tpl.data()[4] = static_cast<std::uint8_t>(prev);
                return tpl.incremental_crc();
              }));
  }

  // --- whole message: per-order fields + checksum --------------------------------------------
  {
    alignas(64) std::uint8_t buf[256];
    print_header("Encode New Order + checksum");
    print_row("writer + snprintf + hw crc", measure([&](std::size_t i, std::uint32_t prev) {
                NewOrderVar v = inputs[i & mask];
                v.seq ^= prev & 1;
                encode_new_order_snprintf(buf, kStatic, v);
                escape(buf);
                return load_le<std::uint32_t>(buf + 180);
              }));
    print_row("writer + digit LUT + hw crc", measure([&](std::size_t i, std::uint32_t prev) {
                NewOrderVar v = inputs[i & mask];
                v.seq ^= prev & 1;
                encode_new_order(buf, kStatic, v);
                escape(buf);
                return load_le<std::uint32_t>(buf + 180);
              }));
    const std::size_t n = tpl.size() - kTrailerSize;
    auto tpl_op = [&](auto crc_fn) {
      return [&, crc_fn](std::size_t i, std::uint32_t prev) {
        NewOrderVar v = inputs[i & mask];
        v.seq ^= prev & 1;
        tpl.patch(v);
        const std::uint32_t c = crc_fn(0, tpl.data(), n);
        store_le<std::uint32_t>(tpl.data() + n, c);
        escape(tpl.data());
        return c;
      };
    };
    print_row("template patch only (no crc)", measure([&](std::size_t i, std::uint32_t prev) {
                NewOrderVar v = inputs[i & mask];
                v.seq ^= prev & 1;
                tpl.patch(v);
                escape(tpl.data());
                return static_cast<std::uint32_t>(tpl.data()[hdr::kSeqNum]);
              }));
    print_row("template + sarwate crc", measure(tpl_op(kSarwate)));
    print_row("template + slice-by-8 crc", measure(tpl_op(kSlice8)));
    print_row("template + hw crc", measure(tpl_op(kHw)));
    print_row("template + incremental crc (reload)", measure([&](std::size_t i, std::uint32_t prev) {
                NewOrderVar v = inputs[i & mask];
                v.seq ^= prev & 1;
                tpl.fill_incremental(v);
                escape(tpl.data());
                return load_le<std::uint32_t>(tpl.data() + n);
              }));
    print_row("template + incremental crc (regs)", measure([&](std::size_t i, std::uint32_t prev) {
                NewOrderVar v = inputs[i & mask];
                v.seq ^= prev & 1;
                tpl.fill_regs(v);
                escape(tpl.data());
                return load_le<std::uint32_t>(tpl.data() + n);
              }));
  }

  // --- receive path: Execution Report ------------------------------------------------------
  {
    alignas(64) std::uint8_t er_buf[512];
    const std::size_t er_len = make_trade_report(er_buf, 2);
    std::printf("\nExecution Report (trade, %zu bytes): receive path\n  %-34s %8s %8s %8s %8s\n", er_len, "variant",
                "chained", "p50", "p99", "p99.9");
    {
      alignas(64) std::uint8_t tmp[512];
      std::memcpy(tmp, er_buf, er_len);
      print_row("crc verify only (hw)", measure([&](std::size_t, std::uint32_t prev) {
                  tmp[hdr::kPossResend] = static_cast<std::uint8_t>(prev & 1);  // dependency on previous result
                  return static_cast<std::uint32_t>(verify_checksum(tmp, er_len));
                }));
    }
    print_row("field walk: test 256 bits", measure([&](std::size_t, std::uint32_t prev) {
                std::uint32_t acc = prev;
                for_each_field_naive(er_buf, er_len, exec_report::kFields,
                                     [&](const FieldRef& f) { acc += f.data[0]; });
                return acc;
              }));
    print_row("field walk: clz over 4 words", measure([&](std::size_t, std::uint32_t prev) {
                std::uint32_t acc = prev;
                for_each_field_unchecked(er_buf, er_len, exec_report::kFields,
                                         [&](const FieldRef& f) { acc += f.data[0]; });
                return acc;
              }));
    print_row("decode_exec_report", measure([&](std::size_t, std::uint32_t prev) {
                ExecReport er;
                decode_exec_report(er_buf, er_len, er);
                return static_cast<std::uint32_t>(er.leaves_qty) + prev;
              }));
    print_row("verify + decode + parse ClOrdID", measure([&](std::size_t, std::uint32_t prev) {
                ExecReport er;
                if (!valid_frame(er_buf, er_len) || !decode_exec_report(er_buf, er_len, er)) return 0u;
                return parse_cl_ord_id(er.cl_ord_id) + prev;
              }));
    // whole session receive path: framing, crc, sequence check, dispatch, decode
    static std::uint8_t stream[64][512];
    std::size_t stream_len[64];
    {
      NullTransport tx;
      BenchApp app;
      SessionConfig cfg;
      cfg.comp_id = "CO99999901";
      cfg.store_reserve_bytes = 1 << 20;
      cfg.store_reserve_msgs = 1 << 12;
      auto s = std::make_unique<BenchSession>(cfg, tx, app);
      std::uint8_t lb[256];
      s->on_connected(0);
      const std::size_t ln = encode_logon_reply(lb, 1, "OCGC", 2);
      s->on_bytes(lb, ln, 0);
      // Messages need consecutive sequence numbers, so they are built in blocks of 64 outside the
      // timed region and each on_bytes call is timed on its own (fenced rdtsc, overhead removed).
      std::uint32_t next_seq = 2;
      const std::size_t n = std::min<std::size_t>(g_iters, 1'000'000);
      std::vector<std::uint32_t> lat;
      lat.reserve(n);
      unsigned aux;
      std::uint64_t total = 0;
      for (std::size_t done = 0; done < n + 20'000; done += 64) {
        for (std::size_t j = 0; j < 64; ++j) stream_len[j] = make_trade_report(stream[j], next_seq++);
        for (std::size_t j = 0; j < 64; ++j) {
          _mm_lfence();
          const auto t0 = __rdtsc();
          _mm_lfence();
          s->on_bytes(stream[j], stream_len[j], done + j);
          const auto t1 = __rdtscp(&aux);
          _mm_lfence();
          if (done >= 20'000) {  // warm-up excluded
            lat.push_back(static_cast<std::uint32_t>(t1 - t0));
            total += t1 - t0;
          }
        }
      }
      std::vector<std::uint32_t> empty(100'000);
      for (auto& e : empty) {
        _mm_lfence();
        const auto t0 = __rdtsc();
        _mm_lfence();
        const auto t1 = __rdtscp(&aux);
        _mm_lfence();
        e = static_cast<std::uint32_t>(t1 - t0);
      }
      std::sort(empty.begin(), empty.end());
      std::sort(lat.begin(), lat.end());
      const double oh = empty[empty.size() / 2];
      auto pct = [&](double q) { return std::max(0.0, lat[static_cast<std::size_t>(q * (lat.size() - 1))] - oh) / tsc_ghz(); };
      print_row("Session::on_bytes (1 report)",
                Stats{(static_cast<double>(total) / lat.size() - oh) / tsc_ghz(), pct(0.5), pct(0.99), pct(0.999)});
      std::printf("  (first column for this row: mean of the fenced per-call times, not chained)\n");
      if (!s->active()) std::printf("  (session closed during benchmark!)\n");
    }
  }

  // --- send path through the session ---------------------------------------------------------
  {
    std::printf("\nSession::send_new_order (fill_regs + send + store)\n  %-34s %8s %8s %8s %8s\n", "variant", "chained",
                "p50", "p99", "p99.9");
    const std::size_t iters = 200'000;
    for (bool prefault : {false, true}) {
      NullTransport tx;
      BenchApp app;
      SessionConfig cfg;
      cfg.comp_id = "CO99999901";
      cfg.store_reserve_bytes = 3 * (iters + 20'000) * 192;
      cfg.store_reserve_msgs = 3 * (iters + 20'000);
      cfg.prefault_store = prefault;
      auto s = std::make_unique<BenchSession>(cfg, tx, app);
      std::uint8_t lb[256];
      s->on_connected(0);
      s->on_bytes(lb, encode_logon_reply(lb, 1, "OCGC", 2), 0);
      NewOrderTemplate t(kStatic);
      print_row(prefault ? "store prefaulted" : "store reserved, not touched", measure([&](std::size_t i, std::uint32_t prev) {
                  NewOrderVar v = inputs[i & mask];
                  v.qty += prev & 1;
                  s->send_new_order(t, v, i);
                  return load_le<std::uint32_t>(t.data() + t.size() - 4);
                }, iters));
    }
  }

  // --- checksum cost vs message length -------------------------------------------------------
  {
    std::printf("\nCRC32C chained ns vs length\n  %6s %10s %10s %10s\n", "bytes", "sarwate", "slice8", "hw");
    alignas(64) std::uint8_t buf[2048];
    std::mt19937 rng(9);
    for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
    for (std::size_t len : {32, 64, 128, 180, 256, 512, 1024}) {
      auto op = [&](auto fn) {
        return [&, fn](std::size_t, std::uint32_t prev) {
          buf[0] = static_cast<std::uint8_t>(prev);
          return fn(0, buf, len);
        };
      };
      std::printf("  %6zu %10.1f %10.1f %10.1f\n", len, measure(op(kSarwate)).chained_ns,
                  measure(op(kSlice8)).chained_ns, measure(op(kHw)).chained_ns);
    }
  }
  return 0;
#endif
}
