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

// Engine vs. reference: the reference applies each message to its own L3
// book, takes the top-5 and publishes when it changed. Without conflation the
// engine's publications must be the identical sequence (every snapshot
// correct, none skipped, none reordered). With conflation every publication
// must equal the reference state at the end of its packet.
template <class Book, int Depth>
void check_engine(const std::vector<Event>& ev, std::uint64_t seed, bool conflate, std::size_t k,
                  const std::vector<std::uint32_t>* fixed_sizes = nullptr) {
  Book b;
  ViewFirstEngine<Book, Depth> eng(b);
  MapStdBook ref;
  std::vector<TopSnapshot> ref_last;
  std::vector<std::pair<SymbolId, TopSnapshot>> expected, got;
  std::vector<int> done;
  sim::Rng rng(seed);
  std::size_t pos = 0, packets = 0, publications = 0, fixed_i = 0;
  const int failures_before = obl::test::failures;
  const auto is_packet_event = [](const Event& e) {
    return static_cast<int>(e.type) < kOrderEventTypes || e.type == EventType::ClearSymbol;
  };
  while (pos < ev.size()) {
    if (!is_packet_event(ev[pos])) {
      eng.apply_admin(ev[pos]);
      apply(ref, ev[pos]);
      ++pos;
      continue;
    }
    // random packet size, heavy tail like real bursts
    const std::uint32_t want = fixed_sizes ? (*fixed_sizes)[fixed_i++]
                               : rng.chance(0.1) ? 20 + static_cast<std::uint32_t>(rng.below(40))
                                                 : 1 + static_cast<std::uint32_t>(rng.below(4));
    std::uint32_t m = 0;
    while (m < want && pos + m < ev.size() && is_packet_event(ev[pos + m])) ++m;

    expected.clear();
    for (std::uint32_t i = 0; i < m; ++i) {
      const Event& e = ev[pos + i];
      apply(ref, e);
      if (ref_last.size() <= e.sym) ref_last.resize(static_cast<std::size_t>(e.sym) + 1);
      TopSnapshot t;
      l3_top(ref, e.sym, t);
      if (!(t == ref_last[e.sym])) {
        ref_last[e.sym] = t;
        expected.emplace_back(e.sym, t);
      }
    }
    got.clear();
    done.assign(m, 0);
    eng.process(
        ev.data() + pos, m, k, conflate, [&](SymbolId s, const TopSnapshot& t) { got.emplace_back(s, t); },
        [&](std::uint32_t i) { ++done[i]; });
    ++packets;
    publications += got.size();

    for (std::uint32_t i = 0; i < m; ++i) CHECK_EQ(done[i], 1);  // every message decided exactly once
    if (!conflate) {
      CHECK_EQ(got.size(), expected.size());
      for (std::size_t j = 0; j < std::min(got.size(), expected.size()); ++j) {
        CHECK_EQ(got[j].first, expected[j].first);
        CHECK(got[j].second == expected[j].second);
      }
    } else {
      for (const auto& [sym, t] : got) {
        TopSnapshot end;
        l3_top(ref, sym, end);
        CHECK(t == end);
      }
    }
    for (std::uint32_t i = 0; i < m; ++i) CHECK(eng.consistent(ev[pos + i].sym));
    pos += m;
    if (obl::test::failures > failures_before) {
      std::fprintf(stderr, "  first divergence in packet ending at event %zu (%s, depth %d, conflate=%d, k=%zu)\n",
                   pos, Book::name().c_str(), Depth, conflate, k);
      return;
    }
  }
  CHECK_EQ(b.state_checksum(), ref.state_checksum());
  CHECK_EQ(b.stats().level_missing, 0u);
  const auto& st = eng.stats();
  CHECK_EQ(st.fallback_refills, 0u);
  if (!conflate) CHECK_EQ(st.deferred_publishes, 0u);
  std::printf("  %-36s depth %2d conflate=%d k=%zu: %llu msgs, %zu publications, %llu overlay hits, %llu refills, "
              "%llu fallback, %llu deferred\n",
              Book::name().c_str(), Depth, conflate, k, (unsigned long long)st.messages, publications,
              (unsigned long long)st.overlay_hits, (unsigned long long)st.refills,
              (unsigned long long)st.fallback_refills, (unsigned long long)st.deferred_publishes);
  (void)packets;
}

// ClearSymbol in the middle of a packet followed by a refill of the same
// symbol: the pre-packet L3 levels and the pre-clear overlay entries must not
// leak into the refilled levels.
void test_clear_then_refill() {
  const SymbolId s = 7;
  const auto add = [&](OrderId id, Price p, Qty q) { return Event{EventType::Add, Side::Buy, false, true, s, id, 0, p, q}; };
  const auto del = [&](OrderId id) { return Event{EventType::Remove, Side::Buy, false, true, s, id, 0, 0, 0}; };
  std::vector<Event> ev = {
      // packet 1: resting bids at 40, 39, 38
      add(1, 40, 100), add(2, 39, 100), add(3, 38, 100),
      // packet 2
      Event{EventType::Execute, Side::Buy, false, true, s, 1, 0, 0, 10},  // order 1 enters the overlay (from L3)
      Event{EventType::ClearSymbol, Side::Buy, false, true, s, 0, 0, 0, 0},
      add(10, 50, 5), add(11, 49, 5), add(12, 48, 5), add(13, 47, 5), add(14, 46, 5), add(15, 45, 5),
      add(16, 40, 7),  // cache (6 levels) is full: truncated
      del(10), del(11),  // 4 levels left -> refill
  };
  const std::vector<std::uint32_t> sizes = {3, 11};
  for (bool conflate : {false, true}) check_engine<ArrayOpenBook, 6>(ev, 9, conflate, 0, &sizes);
}

}  // namespace

int main() {
  test_clear_then_refill();
  // deep books (refills rare) and thin books (orders reused within packets, frequent refills, clears)
  const auto deep = generate(11, 400, 0.0);
  const auto thin = generate(12, 15, 0.0005);
  for (bool conflate : {false, true})
    for (std::size_t k : {std::size_t(0), std::size_t(8)}) {
      check_engine<ArrayOpenBook, 10>(deep, 1, conflate, k);
      check_engine<ArrayOpenBook, 10>(thin, 2, conflate, k);
      check_engine<MapStdBook, 10>(thin, 3, conflate, k);
      // a 6-level cache drops below the published 5 constantly: stresses the refill path
      check_engine<ArrayOpenBook, 6>(deep, 4, conflate, k);
      check_engine<ArrayOpenBook, 6>(thin, 5, conflate, k);
      // exactly the published depth: every cleared level forces a refill
      check_engine<ArrayOpenBook, 5>(deep, 6, conflate, k);
      check_engine<ArrayOpenBook, 5>(thin, 7, conflate, k);
      // aggregate-only books under the engine
      check_engine<ArrayInlineAggBook, 6>(thin, 8, conflate, k);
      check_engine<ArrayOpenAggBook, 10>(deep, 9, conflate, k);
    }
  return test_result("test_view");
}
