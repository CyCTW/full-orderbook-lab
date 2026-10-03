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
#include <random>
#include <string_view>
#include <vector>

#include "obl/gw/ocgc/order_template.hpp"

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
Stats measure(Op&& op) {
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
