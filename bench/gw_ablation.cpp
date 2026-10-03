// Each send-path optimization against the straightforward version it replaced, one at a time,
// same machine, same method: fenced rdtsc around every call, timer overhead subtracted,
// p50 / p99 / p99.9 in ns.
//
//   A  request ID -> order slot: dense array vs std::unordered_map
//   B  Client Order ID text: fixed 8 digits as one word vs variable length (to_chars / snprintf)
//   C  hot / cold split: 64-byte order record vs a 128-byte record with the cold fields inline
//   D  open exposure: maintained running total vs summing live orders on every check
//   E  gross open notional: one running total vs a loop over symbols
//   F  counters: single-writer relaxed store vs atomic fetch_add
//   G  "send first": time until the bytes are handed over with bookkeeping after vs before
//   H  fully naive encoder (Writer + snprintf + table CRC) for reference
//
// usage: obl_gw_ablation [--iters N] [--cpu C]

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "obl/gw/instrument.hpp"
#include "obl/gw/metrics.hpp"
#include "obl/gw/ocgc/order_template.hpp"
#include "obl/gw/ocgc/session.hpp"
#include "obl/gw/order_state.hpp"
#include "obl/gw/spsc.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

std::size_t g_iters = 300'000;
std::uint64_t g_oh = 0;

double tsc_ghz() {
  static const double ghz = [] {
    using Clock = std::chrono::steady_clock;
    const auto w0 = Clock::now();
    const auto c0 = __rdtsc();
    while (Clock::now() - w0 < std::chrono::milliseconds(200)) {
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - w0).count();
    return static_cast<double>(__rdtsc() - c0) / ns;
  }();
  return ghz;
}

void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

inline void escape(const void* p) { asm volatile("" : : "r"(p) : "memory"); }

struct Row {
  double p50, p99, p999;
};

Row summarize(std::vector<std::uint32_t>& c) {
  std::sort(c.begin(), c.end());
  auto q = [&](double p) {
    const double v = static_cast<double>(c[static_cast<std::size_t>(p * (c.size() - 1))]) - static_cast<double>(g_oh);
    return std::max(0.0, v) / tsc_ghz();
  };
  return {q(0.5), q(0.99), q(0.999)};
}

// Times f(i) per call. setup(i) runs untimed before each call.
template <class F, class Setup>
Row measure(F&& f, Setup&& setup, std::size_t iters = 0) {
  if (!iters) iters = g_iters;
  const std::size_t warm = std::min<std::size_t>(iters / 10, 20'000);
  std::vector<std::uint32_t> c;
  c.reserve(iters);
  unsigned aux;
  for (std::size_t i = 0; i < iters + warm; ++i) {
    setup(i);
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    f(i);
    const auto t1 = __rdtscp(&aux);
    _mm_lfence();
    if (i >= warm) c.push_back(static_cast<std::uint32_t>(t1 - t0));
  }
  return summarize(c);
}
template <class F>
Row measure(F&& f, std::size_t iters = 0) {
  return measure(std::forward<F>(f), [](std::size_t) {}, iters);
}

void header(const char* title) {
  std::printf("\n%s\n  %-46s %8s %8s %8s\n", title, "variant", "p50", "p99", "p99.9");
}
void row(const char* name, const Row& r) { std::printf("  %-46s %8.1f %8.1f %8.1f\n", name, r.p50, r.p99, r.p999); }

std::uint64_t timer_overhead() {
  std::vector<std::uint32_t> v(100'000);
  unsigned aux;
  for (auto& x : v) {
    _mm_lfence();
    const auto a = __rdtsc();
    _mm_lfence();
    const auto b = __rdtscp(&aux);
    _mm_lfence();
    x = static_cast<std::uint32_t>(b - a);
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// --- A: request ID -> slot ---------------------------------------------------------------------

void bench_id_lookup() {
  const std::size_t n = g_iters;
  header("A  request ID -> order slot (send: insert, report: look up a recent ID)");
  std::mt19937 rng(1);
  std::vector<std::uint32_t> probe(n + 40'000);  // measure() also makes warm-up calls
  for (std::size_t i = 0; i < probe.size(); ++i) probe[i] = static_cast<std::uint32_t>(i - std::min<std::size_t>(i, rng() % 2000));
  {
    std::unordered_map<std::uint32_t, std::uint32_t> m;
    row("unordered_map, no reserve: insert", measure([&](std::size_t i) { m.emplace(10'000'000 + i, i); }, n));
    volatile std::uint32_t sink = 0;
    row("unordered_map: look up", measure([&](std::size_t i) { sink = m.find(10'000'000 + probe[i])->second; }, n));
  }
  {
    std::unordered_map<std::uint32_t, std::uint32_t> m;
    m.reserve(2 * n);
    row("unordered_map, reserved: insert", measure([&](std::size_t i) { m.emplace(10'000'000 + i, i); }, n));
  }
  {
    std::vector<std::uint32_t> a(2 * n);  // touched up front, like OrderTable
    std::uint32_t next = 10'000'000;
    row("dense array (ID - base = index): insert", measure([&](std::size_t i) { a[next++ - 10'000'000] = static_cast<std::uint32_t>(i); }, n));
    volatile std::uint32_t sink = 0;
    row("dense array: look up", measure([&](std::size_t i) {
          const std::uint32_t k = 10'000'000 + probe[i] - 10'000'000;
          sink = k < next - 10'000'000 ? a[k] : 0;
        }, n));
  }
}

// --- B: Client Order ID formatting ---------------------------------------------------------------

void bench_clordid() {
  header("B  Client Order ID into the message (8-byte span)");
  alignas(64) std::uint8_t buf[64] = {};
  row("snprintf, IDs from 1 (variable length)", measure([&](std::size_t i) {
        char t[24];
        const int n = std::snprintf(t, sizeof t, "%u", static_cast<unsigned>(1 + i));
        std::memset(buf, 0, 8);
        std::memcpy(buf, t, static_cast<std::size_t>(n));
        escape(buf);
      }));
  row("to_chars, IDs from 1 (variable length)", measure([&](std::size_t i) {
        std::memset(buf, 0, 8);
        std::to_chars(reinterpret_cast<char*>(buf), reinterpret_cast<char*>(buf) + 8, 1 + i);
        escape(buf);
      }));
  row("digit-pair LUT, fixed 8 digits, 4 x 2-byte stores", measure([&](std::size_t i) {
        const auto id = static_cast<std::uint32_t>(10'000'000 + i);
        char* o = reinterpret_cast<char*>(buf);
        ocgc::detail::put2(o, id / 1000000);
        ocgc::detail::put2(o + 2, id / 10000 % 100);
        ocgc::detail::put2(o + 4, id / 100 % 100);
        ocgc::detail::put2(o + 6, id % 100);
        escape(buf);
      }));
  row("fixed 8 digits built as one word, 1 store", measure([&](std::size_t i) {
        const auto id = static_cast<std::uint32_t>(10'000'000 + i);
        auto pair = [](std::uint32_t v) {
          std::uint16_t p;
          std::memcpy(&p, ocgc::detail::kDigitPairs + 2 * v, 2);
          return std::uint64_t{p};
        };
        const std::uint64_t w =
            pair(id / 1000000) | pair(id / 10000 % 100) << 16 | pair(id / 100 % 100) << 32 | pair(id % 100) << 48;
        store_le<std::uint64_t>(buf, w);
        escape(buf);
      }));
}

// --- C: hot / cold split ---------------------------------------------------------------------------

struct alignas(64) Hot {  // what OrderTable keeps per order on the hot path
  std::int64_t price, qty, cum, leaves, amend_price, amend_qty;
  std::uint32_t symbol, req_id, pending_req;
  std::uint8_t state, side, pending, tif;
};
static_assert(sizeof(Hot) == 64);
struct alignas(64) Fat {  // the same plus what OrderTable keeps in the cold array, inline
  std::int64_t price, qty, cum, leaves, amend_price, amend_qty;
  std::uint32_t symbol, req_id, pending_req;
  std::uint8_t state, side, pending, tif;
  std::uint64_t tag;
  std::uint32_t next, prev;
  std::uint8_t route, exch_len;
  char exch_id[22];
  char text[16];
};
static_assert(sizeof(Fat) == 128);

template <class T>
Row random_update(std::size_t n_orders) {
  std::vector<T> v(n_orders);
  std::vector<std::uint32_t> idx(g_iters + 30'000);
  std::mt19937 rng(3);
  for (auto& x : idx) x = static_cast<std::uint32_t>(rng() % n_orders);
  // a fill report: read state, update cum / leaves
  return measure([&](std::size_t i) {
    T& o = v[idx[i]];
    if (o.state < 3) {
      o.cum += 100;
      o.leaves -= 100;
    }
    escape(&o);
  });
}

void bench_hot_cold() {
  header("C  report updates a random order: 64-byte record vs 128-byte (cold fields inline)");
  for (std::size_t n : {4'096u, 16'384u, 65'536u, 262'144u, 1'048'576u}) {
    char a[96], b[96];
    std::snprintf(a, sizeof a, "%7zu orders, 64 B  (%6.1f MiB)", n, n * 64 / 1048576.0);
    std::snprintf(b, sizeof b, "%7zu orders, 128 B (%6.1f MiB)", n, n * 128 / 1048576.0);
    row(a, random_update<Hot>(n));
    row(b, random_update<Fat>(n));
  }
}

// --- D, E: exposure and gross notional -------------------------------------------------------------

void bench_exposure() {
  header("D  open buy quantity for a risk check: running total vs sum over live orders");
  for (int live : {10, 100, 1000}) {
    std::vector<Hot> orders(live);
    for (int i = 0; i < live; ++i) {
      orders[i].leaves = 100;
      orders[i].side = static_cast<std::uint8_t>(i & 1);
    }
    volatile std::int64_t sink = 0;
    char name[64];
    std::snprintf(name, sizeof name, "sum over %d live orders", live);
    row(name, measure([&](std::size_t) {
          std::int64_t s = 0;
          for (const Hot& o : orders)
            if (o.side == 0) s += o.leaves;
          sink = s;
        }));
  }
  Position p;
  p.open_buy_qty = 12345;
  volatile std::int64_t sink = 0;
  row("running total (Position::open_buy_qty)", measure([&](std::size_t) {
        sink = p.open_buy_qty;
        escape(&p);
      }));

  header("E  gross open notional for a risk check: running total vs loop over symbols");
  for (int syms : {10, 100, 1000}) {
    std::vector<Position> pos(syms);
    char name[64];
    std::snprintf(name, sizeof name, "loop over %d symbols", syms);
    row(name, measure([&](std::size_t) {
          std::int64_t g = 0;
          for (const Position& q : pos) g += q.open_buy_notional + q.open_sell_notional;
          sink = g;
        }));
  }
  std::int64_t gross = 777;
  row("running total (OrderTable::gross_open_notional)", measure([&](std::size_t) {
        sink = gross;
        escape(&gross);
      }));
}

// --- F: counters -----------------------------------------------------------------------------------

void bench_counters() {
  header("F  10 counter increments (one per send-path event)");
  std::array<std::atomic<std::uint64_t>, 10> a{};
  row("atomic fetch_add (lock xadd)", measure([&](std::size_t) {
        for (auto& x : a) x.fetch_add(1, std::memory_order_relaxed);
      }));
  std::array<Counter, 10> c{};
  row("single-writer relaxed load + store", measure([&](std::size_t) {
        for (auto& x : c) x.inc();
      }));
}

// --- G: send first, bookkeeping after ---------------------------------------------------------------

void bench_send_first() {
  header("G  fill + send: time until the bytes are handed over");
  const NewOrderStatic st{"CO99999901", "1234", "700", "ABC123.2568", WireSide::Buy, 20261003};
  NewOrderTemplate tpl(st);
  MessageStore store;
  store.reserve((g_iters + 40'000) * 192, g_iters + 40'000, true);
  SpscRing audit(1u << 27);  // stands in for the audit log copy
  std::array<Counter, 3> cnt{};
  std::uint64_t sent_tsc = 0;
  auto send = [&](const std::uint8_t* m) {
    escape(m);
    sent_tsc = __rdtsc();
  };
  auto bookkeeping = [&](std::uint32_t seq, const std::uint8_t* m, std::size_t n) {
    store.append(seq, m, n);
    if (std::uint8_t* p = audit.reserve(1, static_cast<std::uint32_t>(n + 12))) {
      std::memcpy(p + 12, m, n);
      audit.commit();
    }
    for (auto& x : cnt) x.inc();
  };
  std::uint32_t seq = 1;
  auto run = [&](bool first) {
    std::vector<std::uint32_t> c;
    unsigned aux;
    for (std::size_t i = 0; i < g_iters + 20'000; ++i) {
      const NewOrderVar v{seq, static_cast<std::uint32_t>(10'000'000 + i), 3'600'000'000ull + i, 400 * kScale,
                          100 * kScale};
      _mm_lfence();
      const auto t0 = __rdtsc();
      _mm_lfence();
      const std::uint8_t* m = tpl.fill_regs(v);
      if (first) {
        send(m);
        bookkeeping(seq, m, tpl.size());
      } else {
        bookkeeping(seq, m, tpl.size());
        send(m);
      }
      ++seq;
      (void)__rdtscp(&aux);
      if (i >= 20'000) c.push_back(static_cast<std::uint32_t>(sent_tsc - t0));
      if (i % 4096 == 0) audit.consume([](std::uint32_t, const std::uint8_t*, std::uint32_t) {});
    }
    return summarize(c);
  };
  row("bookkeeping before send (store, audit, counters)", run(false));
  row("send first, bookkeeping after", run(true));

  header("G' message store append: memory reserved only vs touched up front");
  for (bool pre : {false, true}) {
    MessageStore s2;
    s2.reserve((g_iters + 40'000) * 192, g_iters + 40'000, pre);
    std::uint32_t q = 1;
    row(pre ? "prefaulted" : "reserve() only (page fault every ~22 messages)",
        measure([&](std::size_t) { s2.append(q++, tpl.data(), tpl.size()); }));
  }
}

// --- H: fully naive encoder --------------------------------------------------------------------------

void bench_naive_encode() {
  header("H  encode one New Order + checksum");
  const NewOrderStatic st{"CO99999901", "1234", "700", "ABC123.2568", WireSide::Buy, 20261003};
  alignas(64) std::uint8_t buf[256];
  auto naive = [&](std::size_t i, auto crc) {
    namespace b = new_order;
    namespace fs = field_size;
    Writer w(buf, MsgType::NewOrder, static_cast<std::uint32_t>(i), st.comp_id);
    char* id = w.alnum_slot(b::ClOrdId, fs::kClOrdId);
    char tmp[64];
    std::snprintf(tmp, sizeof tmp, "%u", static_cast<unsigned>(1 + i));
    std::memcpy(id, tmp, std::strlen(tmp));
    w.alnum(b::SubmittingBrokerId, st.submitting_broker_id, fs::kBrokerId);
    w.alnum(b::SecurityId, st.security_id, fs::kSecurityId);
    w.u8(b::SecurityIdSource, kSecurityIdSourceExchangeSymbol);
    w.alnum(b::SecurityExchange, "XHKG", fs::kSecurityExchange);
    char* t = w.alnum_slot(b::TransactTime, fs::kTransactTime);
    const unsigned secs = static_cast<unsigned>(3600 + i / 1000000 % 80000);
    std::snprintf(tmp, sizeof tmp, "%08u-%02u:%02u:%02u.%06u", st.trade_date, secs / 3600, secs / 60 % 60, secs % 60,
                  static_cast<unsigned>(i % 1000000));
    std::memcpy(t, tmp, 24);
    w.u8(b::Side, 1);
    w.u8(b::OrdType, 2);
    w.decimal(b::Price, 400 * kScale);
    w.decimal(b::OrderQty, 100 * kScale);
    w.u16(b::DisclosureInstructions, kDisclosureNone);
    w.alnum(b::SubmittingBcan, st.bcan, fs::kBcan);
    const std::size_t n = w.body_offset() + 4;
    store_le<std::uint16_t>(buf + hdr::kLength, static_cast<std::uint16_t>(n));
    store_le<std::uint32_t>(buf + n - 4, crc(0, buf, n - 4));
    escape(buf);
  };
  row("Writer + snprintf + table CRC (1 byte/step)",
      measure([&](std::size_t i) { naive(i, [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_sarwate(c, p, n); }); }));
  row("Writer + snprintf + hardware CRC",
      measure([&](std::size_t i) { naive(i, [](std::uint32_t c, const void* p, std::size_t n) { return crc32c_hw(c, p, n); }); }));
  NewOrderTemplate tpl(st);
  row("template + fill_regs (registers, incremental CRC)", measure([&](std::size_t i) {
        tpl.fill_regs({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(10'000'000 + i), 3'600'000'000ull + i,
                       400 * kScale, 100 * kScale});
        escape(tpl.data());
      }));
}

}  // namespace

int main(int argc, char** argv) {
  int cpu = 2;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--iters" && i + 1 < argc) g_iters = std::strtoull(argv[++i], nullptr, 10);
    if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
  }
  pin(cpu);
  g_oh = timer_overhead();
  std::printf("tsc %.3f GHz, %zu calls per row, cpu %d, ns per call\n", tsc_ghz(), g_iters, cpu);
  bench_naive_encode();
  bench_clordid();
  bench_id_lookup();
  bench_hot_cold();
  bench_exposure();
  bench_counters();
  bench_send_first();
  return 0;
}
