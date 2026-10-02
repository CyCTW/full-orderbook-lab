// obl_burst: latency of messages inside bursty packets.
//
//   obl_burst --events X.ev [--packets X.pkt] [--book dense|dense-ankerl|map]
//             [--publish-ns N] [--k 8] [--big 20] [--top5]
//
// Each packet is treated as arriving all at once (no backlog between packets),
// so a message's latency is the time from the start of processing its packet
// until its effect has been published downstream. "Publishing" = read the
// symbol's best bid/ask and write a top-of-book record into an output ring,
// plus an optional busy-wait of --publish-ns to stand in for downstream work
// (strategy callback, cross-thread queue, ...).
//
// Strategies compared:
//   per-message          apply, publish, apply, publish, ...
//   prefetch             same, with a rolling two-stage prefetch over the
//                        rest of the packet (index slot 2K ahead, order node K ahead)
//   conflate             apply all, then publish each touched symbol once;
//                        every message's latency is its symbol's publish time
//   prefetch + conflate  both
//
// With --top5 every strategy publishes the top-5 depth snapshot (both sides)
// only when it changed, and four "view first" strategies are added: the
// top-N depth is updated from the message (order looked up when needed) and
// published before the L3 book is touched; the L3 update runs afterwards
// (include/obl/book/depth_view.hpp). "pkt done" is when the whole packet,
// including deferred L3 work, has been processed.

#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../tools/args.hpp"
#include "obl/book/depth_view.hpp"
#include "obl/book/events.hpp"
#include "obl/book/variants.hpp"

namespace {

using namespace obl;
using book::Event;
using Clock = std::chrono::steady_clock;

double tsc_ghz() {
  static const double ghz = [] {
    const auto t0 = Clock::now();
    const auto c0 = __rdtsc();
    while (Clock::now() - t0 < std::chrono::milliseconds(200)) {
    }
    const auto c1 = __rdtsc();
    return (c1 - c0) / std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
  }();
  return ghz;
}

struct TopOfBook {
  SymbolId sym;
  Price bid, ask;
  std::uint64_t bid_qty, ask_qty;
};

class Publisher {
 public:
  explicit Publisher(std::uint64_t spin_cycles) : ring_(1 << 14), spin_(spin_cycles) {}

  template <class Book>
  void publish(const Book& b, SymbolId s) {
    const auto* sb = b.book(s);
    const book::Level* bid = sb ? sb->bids.best() : nullptr;
    const book::Level* ask = sb ? sb->asks.best() : nullptr;
    ring_[w_++ & (ring_.size() - 1)] = {s, bid ? bid->price : 0, ask ? ask->price : 0, bid ? bid->qty : 0,
                                        ask ? ask->qty : 0};
    if (spin_) {
      const auto t = __rdtsc();
      while (__rdtsc() - t < spin_) _mm_pause();
    }
  }
  void publish_top(const book::TopSnapshot& t) {
    top_ring_[w_++ & (top_ring_.size() - 1)] = t;
    if (spin_) {
      const auto t0 = __rdtsc();
      while (__rdtsc() - t0 < spin_) _mm_pause();
    }
  }
  std::uint64_t published() const { return w_; }

 private:
  std::vector<TopOfBook> ring_;
  std::vector<book::TopSnapshot> top_ring_ = std::vector<book::TopSnapshot>(1 << 12);
  std::uint64_t w_ = 0;
  std::uint64_t spin_;
};

enum class Mode {
  PerMessage,
  Prefetch,
  Conflate,
  PrefetchConflate,
  ViewFirst,
  ViewFirstPrefetch,
  ViewFirstConflate,
  ViewFirstPrefetchConflate
};
const char* mode_name(Mode m) {
  switch (m) {
    case Mode::PerMessage: return "per-message";
    case Mode::Prefetch: return "prefetch";
    case Mode::Conflate: return "conflate";
    case Mode::PrefetchConflate: return "prefetch + conflate";
    case Mode::ViewFirst: return "view-first";
    case Mode::ViewFirstPrefetch: return "view-first + prefetch";
    case Mode::ViewFirstConflate: return "view-first + conflate";
    case Mode::ViewFirstPrefetchConflate: return "view-first + pf + confl";
  }
  return "?";
}

bool is_order_event(const Event& e) { return static_cast<int>(e.type) < book::kOrderEventTypes; }

struct Stats {
  std::vector<std::uint32_t> all, big_all, big_last, big_first, big_pkt_done;
  std::uint64_t refills = 0, deferred = 0;
  double pos_sum[64] = {};
  std::uint64_t pos_cnt[64] = {};
  double seconds = 0;
  std::uint64_t publishes = 0, checksum = 0;
};

template <class Book>
Stats run(Mode mode, const std::vector<Event>& ev, const std::vector<std::uint32_t>& sizes, std::size_t k,
          std::uint32_t big, std::uint64_t spin) {
  Stats st;
  st.all.reserve(ev.size());
  auto b = std::make_unique<Book>();
  Publisher pub(spin);
  std::vector<std::uint32_t> lat(1 << 16);
  std::vector<SymbolId> dirty;
  std::vector<std::uint32_t> sym_done;
  const bool prefetch = mode == Mode::Prefetch || mode == Mode::PrefetchConflate;
  const bool conflate = mode == Mode::Conflate || mode == Mode::PrefetchConflate;
  const auto t_begin = Clock::now();
  std::size_t pos = 0;
  for (const std::uint32_t n : sizes) {
    const Event* e = ev.data() + pos;
    pos += n;
    if (!is_order_event(e[0])) {  // SetTick / ClearSymbol: not a market data burst
      for (std::uint32_t i = 0; i < n; ++i) book::apply(*b, e[i]);
      continue;
    }
    if (lat.size() < n) lat.resize(n);
    const auto t0 = __rdtsc();
    if (prefetch)
      for (std::uint32_t i = 0; i < std::min<std::size_t>(n, 2 * k); ++i) b->prefetch_index(e[i].sym, e[i].id);
    dirty.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
      if (prefetch) {
        if (i + 2 * k < n) b->prefetch_index(e[i + 2 * k].sym, e[i + 2 * k].id);
        if (i + k < n && book::references_order(e[i + k])) b->prefetch_order(e[i + k].sym, e[i + k].id);
      }
      book::apply(*b, e[i]);
      if (conflate) {
        if (std::find(dirty.begin(), dirty.end(), e[i].sym) == dirty.end()) dirty.push_back(e[i].sym);
      } else {
        pub.publish(*b, e[i].sym);
        lat[i] = static_cast<std::uint32_t>(__rdtsc() - t0);
      }
    }
    if (conflate) {
      sym_done.resize(dirty.size());
      for (std::size_t d = 0; d < dirty.size(); ++d) {
        pub.publish(*b, dirty[d]);
        sym_done[d] = static_cast<std::uint32_t>(__rdtsc() - t0);
      }
      for (std::uint32_t i = 0; i < n; ++i)
        lat[i] = sym_done[std::find(dirty.begin(), dirty.end(), e[i].sym) - dirty.begin()];
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      st.all.push_back(lat[i]);
      if (n >= big) {
        st.big_all.push_back(lat[i]);
        st.pos_sum[std::min<std::uint32_t>(i, 63)] += lat[i];
        ++st.pos_cnt[std::min<std::uint32_t>(i, 63)];
      }
    }
    if (n >= big) {
      st.big_last.push_back(lat[n - 1]);
      st.big_first.push_back(lat[0]);
    }
  }
  st.seconds = std::chrono::duration<double>(Clock::now() - t_begin).count();
  st.publishes = pub.published();
  st.checksum = b->checksum();
  return st;
}

// --top5: publish the top-5 snapshot only when it changed.
template <class Book, int CacheDepth>
Stats run_top5(Mode mode, const std::vector<Event>& ev, const std::vector<std::uint32_t>& sizes, std::size_t k,
               std::uint32_t big, std::uint64_t spin) {
  Stats st;
  st.all.reserve(ev.size());
  auto b = std::make_unique<Book>();
  book::ViewFirstEngine<Book, CacheDepth> eng(*b);
  Publisher pub(spin);
  std::vector<book::TopSnapshot> last;
  std::vector<std::uint32_t> lat(1 << 16);
  std::vector<SymbolId> dirty;
  std::vector<std::uint32_t> sym_done;
  const bool view = mode >= Mode::ViewFirst;
  const bool prefetch = mode == Mode::Prefetch || mode == Mode::PrefetchConflate || mode == Mode::ViewFirstPrefetch ||
                        mode == Mode::ViewFirstPrefetchConflate;
  const bool conflate = mode == Mode::Conflate || mode == Mode::PrefetchConflate || mode == Mode::ViewFirstConflate ||
                        mode == Mode::ViewFirstPrefetchConflate;
  const auto publish_l3 = [&](SymbolId s) {
    if (last.size() <= s) last.resize(static_cast<std::size_t>(s) + 1);
    book::TopSnapshot t;
    book::l3_top(*b, s, t);
    if (t == last[s]) return;
    last[s] = t;
    pub.publish_top(t);
  };
  const auto t_begin = Clock::now();
  std::size_t pos = 0;
  for (const std::uint32_t n : sizes) {
    const Event* e = ev.data() + pos;
    pos += n;
    if (!is_order_event(e[0])) {
      for (std::uint32_t i = 0; i < n; ++i) {
        if (view) eng.apply_admin(e[i]);
        else book::apply(*b, e[i]);
      }
      continue;
    }
    if (lat.size() < n) lat.resize(n);
    const auto t0 = __rdtsc();
    if (view) {
      eng.process(
          e, n, prefetch ? k : 0, conflate, [&](SymbolId, const book::TopSnapshot& t) { pub.publish_top(t); },
          [&](std::uint32_t i) { lat[i] = static_cast<std::uint32_t>(__rdtsc() - t0); });
    } else {
      if (prefetch)
        for (std::uint32_t i = 0; i < std::min<std::size_t>(n, 2 * k); ++i) b->prefetch_index(e[i].sym, e[i].id);
      dirty.clear();
      for (std::uint32_t i = 0; i < n; ++i) {
        if (prefetch) {
          if (i + 2 * k < n) b->prefetch_index(e[i + 2 * k].sym, e[i + 2 * k].id);
          if (i + k < n && book::references_order(e[i + k])) b->prefetch_order(e[i + k].sym, e[i + k].id);
        }
        book::apply(*b, e[i]);
        if (conflate) {
          if (std::find(dirty.begin(), dirty.end(), e[i].sym) == dirty.end()) dirty.push_back(e[i].sym);
        } else {
          publish_l3(e[i].sym);
          lat[i] = static_cast<std::uint32_t>(__rdtsc() - t0);
        }
      }
      if (conflate) {
        sym_done.resize(dirty.size());
        for (std::size_t d = 0; d < dirty.size(); ++d) {
          publish_l3(dirty[d]);
          sym_done[d] = static_cast<std::uint32_t>(__rdtsc() - t0);
        }
        for (std::uint32_t i = 0; i < n; ++i)
          lat[i] = sym_done[std::find(dirty.begin(), dirty.end(), e[i].sym) - dirty.begin()];
      }
    }
    const auto done = static_cast<std::uint32_t>(__rdtsc() - t0);
    for (std::uint32_t i = 0; i < n; ++i) {
      st.all.push_back(lat[i]);
      if (n >= big) {
        st.big_all.push_back(lat[i]);
        st.pos_sum[std::min<std::uint32_t>(i, 63)] += lat[i];
        ++st.pos_cnt[std::min<std::uint32_t>(i, 63)];
      }
    }
    if (n >= big) {
      st.big_last.push_back(lat[n - 1]);
      st.big_first.push_back(lat[0]);
      st.big_pkt_done.push_back(done);
    }
  }
  st.seconds = std::chrono::duration<double>(Clock::now() - t_begin).count();
  st.publishes = pub.published();
  st.checksum = b->checksum();
  st.refills = eng.stats().refills;
  st.deferred = eng.stats().deferred_publishes;
  return st;
}

double pct(std::vector<std::uint32_t>& v, double q) {
  if (v.empty()) return 0;
  const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  return v[i] / tsc_ghz();
}

template <class Book>
void run_all(const std::vector<Event>& ev, const std::vector<std::uint32_t>& sizes, std::size_t k, std::uint32_t big,
             double publish_ns, bool top5, int cache_depth) {
  const std::uint64_t spin = static_cast<std::uint64_t>(publish_ns * tsc_ghz());
  if (top5) std::printf("\nview-first depth cache: %d levels per side", cache_depth >= 32 ? 32 : 10);
  std::printf("\nbook: %s, publish = %s%s\n", Book::name().c_str(),
              top5 ? "top-5 snapshot when changed" : "top-of-book record per message",
              publish_ns > 0 ? (" + " + std::to_string(static_cast<int>(publish_ns)) + " ns downstream work").c_str() : "");
  std::printf("%-24s | %22s | %29s | %22s | %22s | %8s %7s\n", "", "all messages", "msgs in packets >= big",
              "last msg of big pkt", "first msg of big pkt", "total", "pubs/");
  std::printf("%-24s | %6s %7s %7s | %6s %7s %7s %6s | %6s %7s %7s | %6s %7s %7s | %8s %7s\n", "strategy", "p50", "p99",
              "p99.9", "p50", "p99", "p99.9", "", "p50", "p99", "p99.9", "p50", "p99", "p99.9", "seconds", "msg");
  std::uint64_t ref = 0;
  std::vector<std::pair<Mode, Stats>> results;
  std::vector<Mode> modes = {Mode::PerMessage, Mode::Prefetch, Mode::Conflate, Mode::PrefetchConflate};
  if (top5)
    for (Mode m : {Mode::ViewFirst, Mode::ViewFirstPrefetch, Mode::ViewFirstConflate, Mode::ViewFirstPrefetchConflate})
      modes.push_back(m);
  for (Mode m : modes) {
    Stats st = !top5 ? run<Book>(m, ev, sizes, k, big, spin)
               : cache_depth >= 32 ? run_top5<Book, 32>(m, ev, sizes, k, big, spin)
                                   : run_top5<Book, 10>(m, ev, sizes, k, big, spin);
    if (!ref) ref = st.checksum;
    std::printf("%-24s | %6.0f %7.0f %7.0f | %6.0f %7.0f %7.0f %6s | %6.0f %7.0f %7.0f | %6.0f %7.0f %7.0f | %8.2f %7.2f%s\n",
                mode_name(m), pct(st.all, 0.5), pct(st.all, 0.99), pct(st.all, 0.999), pct(st.big_all, 0.5),
                pct(st.big_all, 0.99), pct(st.big_all, 0.999), "", pct(st.big_last, 0.5), pct(st.big_last, 0.99),
                pct(st.big_last, 0.999), pct(st.big_first, 0.5), pct(st.big_first, 0.99), pct(st.big_first, 0.999),
                st.seconds, double(st.publishes) / st.all.size(), st.checksum == ref ? "" : "  <-- BOOK MISMATCH");
    results.emplace_back(m, std::move(st));
  }
  if (top5) {
    std::printf("\nwhole-packet completion (incl. deferred L3 work), packets >= %u msgs; view-first refills / deferred decisions:\n",
                big);
    for (auto& [m, st] : results)
      std::printf("%-24s pkt done p50 %7.0f p99 %7.0f ns   refills %llu, deferred %llu\n", mode_name(m),
                  pct(st.big_pkt_done, 0.5), pct(st.big_pkt_done, 0.99), (unsigned long long)st.refills,
                  (unsigned long long)st.deferred);
  }
  std::printf("\nmean latency (ns) by position in packets of >= %u messages:\n%-24s", big, "position");
  const int shown[] = {0, 4, 9, 14, 19, 29, 39, 49, 63};
  for (int p : shown) std::printf(" %7d%s", p + 1, p == 63 ? "+" : "");
  std::printf("\n");
  for (auto& [m, st] : results) {
    std::printf("%-24s", mode_name(m));
    for (int p : shown) std::printf(" %7.0f", st.pos_cnt[p] ? st.pos_sum[p] / st.pos_cnt[p] / tsc_ghz() : 0.0);
    std::printf("\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  tools::Args args(argc, argv);
  if (!args.has("events")) {
    std::puts("usage: obl_burst --events X.ev [--packets X.pkt] [--book dense|dense-ankerl|map] [--publish-ns N]\n"
              "                 [--k 8] [--big 20] [--top5 [--cache-depth 10|32]]");
    return 1;
  }
  const std::string evp = args.str("events", "");
  const std::vector<Event> ev = book::load_events(evp);
  std::vector<std::uint32_t> sizes;
  const std::string pkp = args.str("packets", evp.substr(0, evp.rfind('.')) + ".pkt");
  try {
    sizes = book::load_packets(pkp);
  } catch (const std::exception&) {
    std::printf("no packet file %s: every event is its own packet\n", pkp.c_str());
    sizes.assign(ev.size(), 1);
  }
  std::uint64_t total = 0;
  for (auto n : sizes) total += n;
  if (total != ev.size()) {
    std::printf("packet file does not match events (%llu vs %zu)\n", (unsigned long long)total, ev.size());
    return 1;
  }
  const std::size_t k = args.u64("k", 8);
  const auto big = static_cast<std::uint32_t>(args.u64("big", 20));
  std::uint64_t big_pkts = 0, big_msgs = 0;
  for (auto n : sizes)
    if (n >= big) ++big_pkts, big_msgs += n;
  std::printf("%zu events in %zu packets; %llu packets with >= %u messages (%.2f%% of messages); tsc %.3f GHz\n",
              ev.size(), sizes.size(), (unsigned long long)big_pkts, big, 100.0 * big_msgs / ev.size(), tsc_ghz());

  const std::string which = args.str("book", "dense");
  const double publish_ns = args.num("publish-ns", 0);
  const bool top5 = args.has("top5");
  const int cache_depth = static_cast<int>(args.u64("cache-depth", 10));  // 10 or 32
  if (which == "dense") run_all<book::ArrayOpenBook>(ev, sizes, k, big, publish_ns, top5, cache_depth);
#ifdef OBL_HAVE_UNORDERED_DENSE
  else if (which == "dense-ankerl") run_all<book::ArrayDenseBook>(ev, sizes, k, big, publish_ns, top5, cache_depth);
#endif
  else if (which == "map") run_all<book::MapStdBook>(ev, sizes, k, big, publish_ns, top5, cache_depth);
  else {
    std::printf("unknown --book %s\n", which.c_str());
    return 1;
  }
  return 0;
}
