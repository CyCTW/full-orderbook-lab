// "View first" engine: after every packet the cached depth must equal the L3
// book, and the last published top-5 must equal the L3 top-5.

#include <vector>

#include "check.hpp"
#include "obl/book/depth_view.hpp"
#include "obl/book/variants.hpp"
#include "obl/sim/generator.hpp"
#include "obl/xdp/decoder.hpp"
#include "obl/xdp/event_recorder.hpp"

using namespace obl;
using namespace obl::book;

namespace {

std::vector<Event> generate(std::uint64_t seed, std::uint32_t target_orders, double clear_prob) {
  struct Sink {
    std::vector<std::vector<std::uint8_t>> packets;
    void packet(std::uint64_t, const std::uint8_t* p, std::size_t n) { packets.emplace_back(p, p + n); }
  } sink;
  sim::GenConfig cfg;
  cfg.symbols = 12;
  cfg.messages = 150'000;
  cfg.target_orders = target_orders;
  cfg.symbol_clear_prob = clear_prob;
  cfg.seed = seed;
  sim::PacketBuilder<Sink> pb(sink, cfg.max_msgs_per_packet, cfg.seed);
  sim::FlowGenerator<sim::PacketBuilder<Sink>> gen(cfg, pb);
  gen.run();
  std::vector<Event> ev;
  xdp::EventRecorder rec(ev);
  xdp::DecodeStats st;
  xdp::SequenceTracker seq;
  for (const auto& p : sink.packets) xdp::process_datagram(seq, 1, p.data(), p.size(), rec, st);
  return ev;
}

template <class Book>
void check_engine(const std::vector<Event>& ev, std::uint64_t seed, bool conflate, std::size_t k) {
  Book b;
  ViewFirstEngine<Book> eng(b);
  std::vector<TopSnapshot> published;
  std::vector<int> done;
  sim::Rng rng(seed);
  std::size_t pos = 0;
  int failures_before = obl::test::failures;
  while (pos < ev.size()) {
    if (static_cast<int>(ev[pos].type) >= kOrderEventTypes && ev[pos].type != EventType::ClearSymbol) {
      eng.apply_admin(ev[pos++]);
      continue;
    }
    // random packet size, heavy tail like real bursts
    std::uint32_t n = rng.chance(0.1) ? 20 + static_cast<std::uint32_t>(rng.below(40)) : 1 + static_cast<std::uint32_t>(rng.below(4));
    std::uint32_t m = 0;
    while (m < n && pos + m < ev.size() &&
           (static_cast<int>(ev[pos + m].type) < kOrderEventTypes || ev[pos + m].type == EventType::ClearSymbol))
      ++m;
    done.assign(m, 0);
    eng.process(
        ev.data() + pos, m, k, conflate,
        [&](SymbolId s, const TopSnapshot& t) {
          if (published.size() <= s) published.resize(s + 1);
          published[s] = t;
        },
        [&](std::uint32_t i) { ++done[i]; });
    for (std::uint32_t i = 0; i < m; ++i) CHECK_EQ(done[i], 1);  // every message decided exactly once
    for (std::uint32_t i = 0; i < m; ++i) {
      const SymbolId s = ev[pos + i].sym;
      CHECK(eng.consistent(s));
      TopSnapshot l3;
      l3_top(b, s, l3);
      const TopSnapshot pub = s < published.size() ? published[s] : TopSnapshot{};
      CHECK(pub == l3);
    }
    pos += m;
    if (obl::test::failures > failures_before) {
      std::fprintf(stderr, "  first divergence near event %zu (%s, conflate=%d, k=%zu)\n", pos, Book::name().c_str(),
                   conflate, k);
      return;
    }
  }
  CHECK_EQ(b.stats().level_missing, 0u);
  const auto& st = eng.stats();
  std::printf("  %-36s conflate=%d k=%zu: %llu msgs, %llu L3 lookups, %llu overlay hits, %llu refills, %llu deferred\n",
              Book::name().c_str(), conflate, k, (unsigned long long)st.messages, (unsigned long long)st.lookups,
              (unsigned long long)st.overlay_hits, (unsigned long long)st.refills,
              (unsigned long long)st.deferred_publishes);
}

}  // namespace

int main() {
  // deep books (refills rare) and thin books (orders reused within packets, frequent refills, clears)
  const auto deep = generate(11, 400, 0.0);
  const auto thin = generate(12, 15, 0.0005);
  for (bool conflate : {false, true})
    for (std::size_t k : {std::size_t(0), std::size_t(8)}) {
      check_engine<ArrayOpenBook>(deep, 1, conflate, k);
      check_engine<ArrayOpenBook>(thin, 2, conflate, k);
      check_engine<MapStdBook>(thin, 3, conflate, k);
    }
  return test_result("test_view");
}
