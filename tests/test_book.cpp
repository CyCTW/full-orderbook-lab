// Book semantics + differential tests: every variant must match a naive
// reference book (std::map + std::list) after every operation, including the
// FIFO queue order at each price level.

#include <list>
#include <map>
#include <tuple>
#include <vector>

#include "check.hpp"
#include "obl/book/events.hpp"
#include "obl/book/variants.hpp"
#include "obl/sim/generator.hpp"
#include "obl/xdp/decoder.hpp"
#include "obl/xdp/event_recorder.hpp"

using namespace obl;
using namespace obl::book;

namespace {

// (symbol, side, price, [(id, qty) in queue order])
using LevelSnap = std::tuple<SymbolId, int, Price, std::vector<std::pair<OrderId, Qty>>>;
using Snapshot = std::vector<LevelSnap>;

class ReferenceBook {
 public:
  void set_tick(SymbolId, Price) {}
  bool add(SymbolId s, OrderId id, Side side, Price p, Qty q) {
    if (where_.count({s, id})) return false;
    auto& lvl = side_map(s, side)[p];
    lvl.push_back({id, q});
    where_[{s, id}] = {side, p};
    return true;
  }
  bool remove(SymbolId s, OrderId id) {
    auto it = where_.find({s, id});
    if (it == where_.end()) return false;
    auto& m = side_map(s, it->second.first);
    auto lit = m.find(it->second.second);
    lit->second.remove_if([&](auto& e) { return e.first == id; });
    if (lit->second.empty()) m.erase(lit);
    where_.erase(it);
    return true;
  }
  bool execute(SymbolId s, OrderId id, Qty q) {
    auto* e = find(s, id);
    if (!e) return false;
    if (q >= e->second) return remove(s, id);
    e->second -= q;
    return true;
  }
  bool modify(SymbolId s, OrderId id, Price p, Qty q, bool keep) {
    auto it = where_.find({s, id});
    if (it == where_.end()) return false;
    if (q == 0) return remove(s, id);
    const Side side = it->second.first;
    if (p == it->second.second && keep) {
      find(s, id)->second = q;
      return true;
    }
    remove(s, id);
    return add(s, id, side, p, q);
  }
  bool replace(SymbolId s, OrderId old_id, OrderId new_id, Side side, Price p, Qty q) {
    const bool r = remove(s, old_id);
    return add(s, new_id, side, p, q) && r;
  }
  void clear_symbol(SymbolId s) {
    for (auto it = where_.begin(); it != where_.end();)
      it = it->first.first == s ? where_.erase(it) : std::next(it);
    books_.erase(s);
  }

  Price price_of(SymbolId s, OrderId id) const {
    auto it = where_.find({s, id});
    return it == where_.end() ? 0 : it->second.second;
  }

  Snapshot snapshot() const {
    Snapshot out;
    for (const auto& [s, b] : books_) {
      for (auto it = b.bids.rbegin(); it != b.bids.rend(); ++it)
        out.emplace_back(s, 0, it->first, std::vector<std::pair<OrderId, Qty>>(it->second.begin(), it->second.end()));
      for (const auto& [p, l] : b.asks)
        out.emplace_back(s, 1, p, std::vector<std::pair<OrderId, Qty>>(l.begin(), l.end()));
    }
    return out;
  }

 private:
  using Lvl = std::list<std::pair<OrderId, Qty>>;
  struct Sym {
    std::map<Price, Lvl> bids, asks;
  };
  std::map<Price, Lvl>& side_map(SymbolId s, Side side) {
    return side == Side::Buy ? books_[s].bids : books_[s].asks;
  }
  std::pair<OrderId, Qty>* find(SymbolId s, OrderId id) {
    auto it = where_.find({s, id});
    if (it == where_.end()) return nullptr;
    for (auto& e : side_map(s, it->second.first)[it->second.second])
      if (e.first == id) return &e;
    return nullptr;
  }
  std::map<SymbolId, Sym> books_;
  std::map<std::pair<SymbolId, OrderId>, std::pair<Side, Price>> where_;
};

template <class Book>
Snapshot snapshot(const Book& b) {
  Snapshot out;
  for (SymbolId s = 0; s < b.symbols(); ++s) {
    const auto* sb = b.book(s);
    const auto take = [&](int side) {
      return [&, side](const Level& l) {
        std::vector<std::pair<OrderId, Qty>> q;
        std::uint64_t total = 0;
        for (std::uint32_t i = l.head; i != kNil; i = b.pool()[i].next) {
          q.push_back({b.pool()[i].id, b.pool()[i].qty});
          total += b.pool()[i].qty;
        }
        CHECK_EQ(total, l.qty);
        CHECK_EQ(q.size(), l.count);
        out.emplace_back(s, side, l.price, std::move(q));
        return true;
      };
    };
    sb->bids.for_each(take(0));
    sb->asks.for_each(take(1));
  }
  return out;
}

template <class Book>
void test_semantics() {
  Book b;
  b.set_tick(1, 10);
  CHECK(b.add(1, 100, Side::Buy, 1000, 10));
  CHECK(b.add(1, 101, Side::Buy, 1000, 20));
  CHECK(b.add(1, 102, Side::Buy, 990, 5));
  CHECK(b.add(1, 200, Side::Sell, 1010, 7));
  CHECK(!b.add(1, 100, Side::Buy, 1000, 1));  // duplicate id
  CHECK(b.add(2, 100, Side::Sell, 5000, 1));  // same id, other symbol is fine

  auto bids = b.depth(1, Side::Buy, 10);
  CHECK_EQ(bids.size(), 2u);
  CHECK_EQ(bids[0].price, 1000);
  CHECK_EQ(bids[0].qty, 30u);
  CHECK_EQ(bids[0].count, 2u);
  CHECK_EQ(b.depth(1, Side::Sell, 10)[0].price, 1010);

  // qty decrease keeping priority: 100 stays ahead of 101
  CHECK(b.modify(1, 100, 1000, 4, true));
  CHECK((b.queue(*b.book(1)->bids.best()) == std::vector<OrderId>{100, 101}));
  CHECK_EQ(b.book(1)->bids.best()->qty, 24u);
  // losing priority moves 100 behind 101
  CHECK(b.modify(1, 100, 1000, 6, false));
  CHECK((b.queue(*b.book(1)->bids.best()) == std::vector<OrderId>{101, 100}));
  // partial then full execution
  CHECK(b.execute(1, 101, 5));
  CHECK_EQ(b.book(1)->bids.best()->qty, 21u);
  CHECK(b.execute(1, 101, 15));
  CHECK((b.queue(*b.book(1)->bids.best()) == std::vector<OrderId>{100}));
  // price change empties the 1000 level, new best is 1005
  CHECK(b.modify(1, 100, 1005, 6, true));
  CHECK_EQ(b.depth(1, Side::Buy, 10).size(), 2u);
  CHECK_EQ(b.book(1)->bids.best()->price, 1005);
  // replace to an off-tick, far-away price (exercises dense-array overflow)
  CHECK(b.replace(1, 102, 103, Side::Buy, 7, 9));
  CHECK_EQ(b.depth(1, Side::Buy, 10).back().price, 7);
  CHECK(b.remove(1, 100));
  CHECK_EQ(b.book(1)->bids.best()->price, 7);
  CHECK(!b.remove(1, 100));
  CHECK_EQ(b.stats().unknown_order, 1u);
  CHECK_EQ(b.order_count(), 3u);

  b.clear_symbol(1);
  CHECK_EQ(b.order_count(), 1u);
  CHECK(b.book(1)->bids.best() == nullptr);
  CHECK(b.book(1)->asks.best() == nullptr);
  CHECK(b.add(1, 100, Side::Buy, 1000, 10));  // ids are free again after the clear
  CHECK_EQ(b.stats().level_missing, 0u);
}

struct RandomOpsConfig {
  std::uint64_t seed;
  int ops;
  Price spread;   // price range in ticks around the anchor
  Price tick;
  double off_tick;
};

template <class Book>
void test_random_vs_reference(const RandomOpsConfig& c) {
  Book b;
  ReferenceBook ref;
  sim::Rng rng(c.seed);
  std::vector<std::pair<SymbolId, OrderId>> live;
  OrderId next = 1;
  for (SymbolId s = 0; s < 3; ++s) b.set_tick(s, c.tick);
  for (int n = 0; n < c.ops; ++n) {
    const SymbolId s = static_cast<SymbolId>(rng.below(3));
    auto price = [&] {
      Price p = 10000 + static_cast<Price>(rng.below(2 * c.spread)) - c.spread;
      p *= c.tick;
      if (rng.chance(c.off_tick)) p += 1 + static_cast<Price>(rng.below(c.tick - 1 > 0 ? c.tick - 1 : 1));
      return std::max<Price>(1, p);
    };
    const Qty q = 1 + static_cast<Qty>(rng.below(500));
    const int op = live.empty() ? 0 : static_cast<int>(rng.below(100));
    if (op < 40) {
      const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
      const Price p = price();
      const bool a = b.add(s, next, side, p, q), r = ref.add(s, next, side, p, q);
      CHECK_EQ(a, r);
      live.push_back({s, next++});
    } else {
      const std::size_t k = rng.below(live.size());
      const auto [ls, id] = live[k];
      if (op < 65) {
        CHECK_EQ(b.remove(ls, id), ref.remove(ls, id));
        live[k] = live.back();
        live.pop_back();
      } else if (op < 80) {
        const bool keep = rng.chance(0.5);
        const Price use = rng.chance(0.5) ? price() : ref.price_of(ls, id);
        CHECK_EQ(b.modify(ls, id, use, q, keep), ref.modify(ls, id, use, q, keep));
      } else if (op < 95) {
        CHECK_EQ(b.execute(ls, id, q), ref.execute(ls, id, q));
      } else if (op < 99) {
        const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
        const Price p = price();
        CHECK_EQ(b.replace(ls, id, next, side, p, q), ref.replace(ls, id, next, side, p, q));
        live[k] = {ls, next++};
      } else {
        b.clear_symbol(ls);
        ref.clear_symbol(ls);
      }
      // ids that were fully executed or cleared stay in `live`; operations on
      // them must be rejected identically by both books.
    }
    if (n % 97 == 0 || n == c.ops - 1) {
      const bool same = snapshot(b) == ref.snapshot();
      CHECK(same);
      if (!same) {
        std::fprintf(stderr, "  diverged at op %d (%s)\n", n, Book::name().c_str());
        return;
      }
    }
  }
  CHECK_EQ(b.stats().level_missing, 0u);
}

// Generated XDP flow -> decode -> every variant ends in the same state with no
// references to unknown orders (validates the generator and the decoder too).
void test_generated_flow() {
  struct Sink {
    std::vector<std::uint8_t> buf;
    std::vector<std::vector<std::uint8_t>> packets;
    void packet(std::uint64_t, const std::uint8_t* p, std::size_t n) { packets.emplace_back(p, p + n); }
  } sink;
  sim::GenConfig cfg;
  cfg.symbols = 30;
  cfg.messages = 400'000;
  cfg.target_orders = 400;
  cfg.symbol_clear_prob = 0.0002;
  sim::PacketBuilder<Sink> pb(sink, cfg.max_msgs_per_packet, cfg.seed);
  sim::FlowGenerator<sim::PacketBuilder<Sink>> gen(cfg, pb);
  gen.run();

  std::vector<Event> events;
  xdp::EventRecorder rec(events);
  xdp::DecodeStats st;
  xdp::SequenceTracker seq;
  for (const auto& p : sink.packets) xdp::process_datagram(seq, 1, p.data(), p.size(), rec, st);
  CHECK_EQ(st.gaps, 0u);
  CHECK_EQ(st.malformed_packets, 0u);
  CHECK_EQ(st.messages, cfg.messages + cfg.symbols + 1);

  ReferenceBook ref;
  for (const auto& e : events) apply(ref, e);
  const Snapshot expect = ref.snapshot();
  std::uint64_t first_sum = 0;
  bool first = true;
  for_each_variant([&]<class Book>() {
    Book b;
    for (const auto& e : events) apply(b, e);
    CHECK_EQ(b.stats().unknown_order, 0u);
    CHECK_EQ(b.stats().duplicate_add, 0u);
    CHECK_EQ(b.stats().over_execution, 0u);
    CHECK_EQ(b.stats().level_missing, 0u);
    CHECK_EQ(b.order_count(), gen.live_orders());
    CHECK(snapshot(b) == expect);
    for (SymbolId sym = 0; sym < b.symbols(); ++sym) {  // generator must never cross the book
      const Level* bid = b.book(sym)->bids.best();
      const Level* ask = b.book(sym)->asks.best();
      CHECK(!bid || !ask || bid->price < ask->price);
      CHECK(!bid || bid->price > 0);
    }
    if (first) first_sum = b.checksum(), first = false;
    CHECK_EQ(b.checksum(), first_sum);
  });
}

}  // namespace

int main() {
  for_each_variant([]<class Book>() {
    test_semantics<Book>();
    test_random_vs_reference<Book>({1, 20000, 20, 1, 0.0});      // narrow, dense
    test_random_vs_reference<Book>({2, 20000, 3000, 10, 0.05});  // wide, off-tick prices
    test_random_vs_reference<Book>({3, 20000, 200000, 1, 0.0});  // beyond the dense window span
  });
  test_generated_flow();
  return test_result("test_book");
}
