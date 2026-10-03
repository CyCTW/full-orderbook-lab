#pragma once
// Multi-symbol, exchange-neutral L3 (market-by-order) book.
//
//   Levels<Side>  : per-side price level container (map / sorted vector / dense array ...)
//   Index         : (symbol, order id) -> order pool index
//
// Feed adapters (XDP today, OMD-C later) translate wire messages into the
// add / modify / execute / remove / replace / clear_symbol calls below.

#include <cstdint>
#include <string>
#include <vector>

#include "obl/book/order_index.hpp"
#include "obl/book/order_pool.hpp"
#include "obl/book/queues.hpp"

namespace obl::book {

struct BookStats {
  std::uint64_t unknown_order = 0;   // modify/execute/delete for an id we do not hold
  std::uint64_t duplicate_add = 0;   // add with an id that is already live
  std::uint64_t over_execution = 0;  // executed quantity exceeded remaining quantity
  std::uint64_t level_missing = 0;   // internal inconsistency; must stay 0
  std::uint64_t bad_symbol = 0;      // symbol id does not fit the packed order node
};

struct LevelView {
  Price price;
  std::uint64_t qty;
  std::uint32_t count;
};

template <template <Side> class LevelsT, class Index, class Queues = ListQueues>
class L3Book {
 public:
  using Bids = LevelsT<Side::Buy>;
  using Asks = LevelsT<Side::Sell>;

  struct SymbolBook {
    Bids bids;
    Asks asks;
  };

  static std::string name() {
    std::string n = std::string(Bids::name) + " + " + Index::name;
    if constexpr (Queues::name != nullptr) n += std::string(" + ") + Queues::name;
    return n;
  }

  L3Book() {
    if constexpr (requires(Index& ix, const OrderPool* p) { ix.attach(p); }) index_.attach(&pool_);
  }
  L3Book(const L3Book&) = delete;  // the index may hold a pointer to pool_
  L3Book& operator=(const L3Book&) = delete;

  void reserve(std::size_t orders, std::size_t symbols) {
    pool_.reserve(orders);
    index_.reserve(orders);
    books_.reserve(symbols);
  }

  void set_tick(SymbolId sym, Price tick) {
    SymbolBook& b = book_for(sym);
    b.bids.set_tick(tick);
    b.asks.set_tick(tick);
  }

  bool add(SymbolId sym, OrderId id, Side side, Price price, Qty qty) {
    if (sym > Order::kMaxSymbol) {
      ++stats_.bad_symbol;
      return false;
    }
    const std::uint32_t i = pool_.alloc();
    pool_[i] = Order::make(id, sym, side, price, qty);  // before insert: compact indexes verify keys via the node
    if (!index_.insert(sym, id, i)) {
      pool_.release(i);
      ++stats_.duplicate_add;
      return false;
    }
    link(book_for(sym), i);
    return true;
  }

  bool remove(SymbolId sym, OrderId id) {
    const std::uint32_t i = index_.erase(sym, id);
    if (i == kNil) {
      ++stats_.unknown_order;
      return false;
    }
    unlink(books_[sym], i);
    pool_.release(i);
    return true;
  }

  // Executed quantity reduces the order; a fully filled order leaves the book.
  bool execute(SymbolId sym, OrderId id, Qty executed) {
    const std::uint32_t i = index_.find(sym, id);
    if (i == kNil) {
      ++stats_.unknown_order;
      return false;
    }
    Order& o = pool_[i];
    if (executed >= o.qty) {
      if (executed > o.qty) ++stats_.over_execution;
      index_.erase(sym, id);
      unlink(books_[sym], i);
      pool_.release(i);
      return true;
    }
    SymbolBook& b = books_[sym];
    const auto reduce = [&](Level& l) { l.qty -= executed; };
    const bool ok = o.side() == Side::Buy ? b.bids.update(o.price, reduce) : b.asks.update(o.price, reduce);
    if (!ok) ++stats_.level_missing;
    o.qty -= executed;
    return true;
  }

  // keep_priority: the exchange says the order kept its queue position
  // (XDP PositionChange == 0). Independently of that flag, a price change or a
  // size increase always loses priority (exchange rule; also covers feeds that
  // leave PositionChange unpopulated).
  bool modify(SymbolId sym, OrderId id, Price price, Qty qty, bool keep_priority) {
    const std::uint32_t i = index_.find(sym, id);
    if (i == kNil) {
      ++stats_.unknown_order;
      return false;
    }
    if (qty == 0) return remove(sym, id);
    SymbolBook& b = books_[sym];
    Order& o = pool_[i];
    if (price == o.price && qty <= o.qty && keep_priority) {
      const Qty old = o.qty;
      const auto resize = [&](Level& l) { l.qty = l.qty - old + qty; };
      const bool ok = o.side() == Side::Buy ? b.bids.update(price, resize) : b.asks.update(price, resize);
      if (!ok) ++stats_.level_missing;
      o.qty = qty;
      return true;
    }
    requeue(b, i, price, qty);
    return true;
  }

  // Replace reuses the order node in place: the index is re-keyed and the
  // order is re-queued at the back of its (possibly new) price level. At an
  // unchanged price the level is never erased and re-created.
  bool replace(SymbolId sym, OrderId old_id, OrderId new_id, Side side, Price price, Qty qty) {
    return replace_impl(sym, old_id, new_id, &side, price, qty);
  }

  // Replace without a side on the wire: the new order keeps the old order's side.
  bool replace(SymbolId sym, OrderId old_id, OrderId new_id, Price price, Qty qty) {
    return replace_impl(sym, old_id, new_id, nullptr, price, qty);
  }

  void clear_symbol(SymbolId sym) {
    if (sym >= books_.size()) return;
    SymbolBook& b = books_[sym];
    if constexpr (Queues::kFifo) {
      const auto drop = [&](const Level& l) {
        queues_.for_each(l, pool_, [&](std::uint32_t i) {
          index_.erase(sym, pool_[i].id);
          pool_.release(i);
        });
        queues_.release(l);
        return true;
      };
      b.bids.for_each(drop);
      b.asks.for_each(drop);
    } else {
      // No per-level order lists: scan the pool (symbol clears are rare).
      for_each_live([&](std::uint32_t i) {
        if (pool_[i].symbol() != sym) return;
        index_.erase(sym, pool_[i].id);
        pool_.release(i);
      });
    }
    b.bids.clear();
    b.asks.clear();
  }

  // ------------------------------------------------------------ prefetch
  // A handler sees several messages per packet, so it can look ahead:
  //   stage 1 (far ahead):  prefetch the index slot of an upcoming order id
  //   stage 2 (near ahead): resolve the id (slot now cached), prefetch the order node
  static constexpr bool kCanPrefetch = requires(const Index& ix) { ix.prefetch(SymbolId{}, OrderId{}); };

  void prefetch_index(SymbolId sym, OrderId id) const {
    if constexpr (kCanPrefetch) index_.prefetch(sym, id);
  }

  void prefetch_order(SymbolId sym, OrderId id) const {
    // A verifying find() on a compact index would itself stall on the node.
    std::uint32_t i;
    if constexpr (requires(const Index& ix) { ix.peek(sym, id); }) i = index_.peek(sym, id);
    else i = index_.find(sym, id);
    if (i != kNil) __builtin_prefetch(&pool_[i]);
  }

  // ------------------------------------------------------------ queries
  // Resting order by id, or nullptr (read-only view of the order node).
  const Order* find_order(SymbolId sym, OrderId id) const {
    const std::uint32_t i = index_.find(sym, id);
    return i == kNil ? nullptr : &pool_[i];
  }

  const SymbolBook* book(SymbolId sym) const { return sym < books_.size() ? &books_[sym] : nullptr; }
  std::size_t symbols() const { return books_.size(); }
  std::size_t order_count() const { return index_.size(); }
  const BookStats& stats() const { return stats_; }
  const OrderPool& pool() const { return pool_; }

  std::vector<LevelView> depth(SymbolId sym, Side side, std::size_t n) const {
    std::vector<LevelView> out;
    const SymbolBook* b = book(sym);
    if (!b) return out;
    const auto take = [&](const Level& l) {
      out.push_back({l.price, l.qty, l.count});
      return out.size() < n;
    };
    if (n == 0) return out;
    if (side == Side::Buy) b->bids.for_each(take);
    else b->asks.for_each(take);
    return out;
  }

  static constexpr bool kQueueOrder = Queues::kFifo;

  // Visits the orders of a level in queue (FIFO) order: f(const Order&).
  template <class F>
    requires Queues::kFifo
  void for_each_order(const Level& l, F&& f) const {
    queues_.for_each(l, pool_, [&](std::uint32_t i) { f(pool_[i]); });
  }

  // Order ids at a level in queue (FIFO) order.
  std::vector<OrderId> queue(const Level& l) const
    requires Queues::kFifo
  {
    std::vector<OrderId> ids;
    for_each_order(l, [&](const Order& o) { ids.push_back(o.id); });
    return ids;
  }

  // Hash over every symbol's full depth including per-level queue order.
  // Two variants processing the same feed must produce the same value.
  // Without queues this falls back to state_checksum().
  std::uint64_t checksum() const {
    if constexpr (!Queues::kFifo) return state_checksum();
    std::uint64_t h = 0x12345678;
    const auto mix = [&](std::uint64_t v) { h = mix64(h ^ v) + 0x9E3779B97F4A7C15ULL; };
    for (SymbolId s = 0; s < books_.size(); ++s) {
      const auto visit = [&](const Level& l) {
        mix(s);
        mix(static_cast<std::uint64_t>(l.price));
        mix(l.qty);
        mix(l.count);
        if constexpr (Queues::kFifo)
          for_each_order(l, [&](const Order& o) {
            mix(o.id);
            mix(o.qty);
          });
        return true;
      };
      mix(0xB1D);
      books_[s].bids.for_each(visit);
      mix(0xA5C);
      books_[s].asks.for_each(visit);
    }
    return h;
  }

  // Queue-order independent: every level (price, qty, count) plus the set of
  // resting orders. Comparable between books with and without queues.
  std::uint64_t state_checksum() const {
    std::uint64_t h = levels_checksum(books_);
    std::uint64_t orders = 0;
    for_each_live([&](std::uint32_t i) {
      const Order& o = pool_[i];
      orders += order_hash(o.symbol(), o.id, o.side(), o.price, o.qty);
    });
    return mix64(h ^ orders);
  }

 private:
  // Visits every live pool slot (a freed slot is no longer what the index maps its key to).
  template <class F>
  void for_each_live(F&& f) const {
    for (std::uint32_t i = 0; i < pool_.capacity(); ++i) {
      const Order& o = pool_[i];
      if (index_.find(o.symbol(), o.id) == i) f(i);
    }
  }

  SymbolBook& book_for(SymbolId sym) {
    if (sym >= books_.size()) books_.resize(static_cast<std::size_t>(sym) + 1);
    return books_[sym];
  }

  bool replace_impl(SymbolId sym, OrderId old_id, OrderId new_id, const Side* side, Price price, Qty qty) {
    const std::uint32_t i = index_.erase(sym, old_id);  // node still holds old_id (compact index verifies it)
    if (i == kNil) {
      ++stats_.unknown_order;
      if (side) add(sym, new_id, *side, price, qty);
      return false;
    }
    SymbolBook& b = books_[sym];
    Order& o = pool_[i];
    if (side && *side != o.side()) {  // side change: not an in-place operation
      unlink(b, i);
      pool_.release(i);
      return add(sym, new_id, *side, price, qty);
    }
    o.id = new_id;  // re-key: the node must hold the new id before the index insert
    if (!index_.insert(sym, new_id, i)) {
      ++stats_.duplicate_add;
      unlink(b, i);
      pool_.release(i);
      return false;
    }
    requeue(b, i, price, qty);
    return true;
  }

  // Moves order i to the back of the queue at `price` with quantity `qty`.
  void requeue(SymbolBook& b, std::uint32_t i, Price price, Qty qty) {
    Order& o = pool_[i];
    if (price == o.price) {
      const auto move_back = [&](Level& l) {
        queues_.unlink(l, pool_, i);
        o.qty = qty;
        queues_.push_back(l, pool_, i);
      };
      const bool ok = o.side() == Side::Buy ? b.bids.update(price, move_back) : b.asks.update(price, move_back);
      if (!ok) ++stats_.level_missing;
      return;
    }
    unlink(b, i);
    o.price = price;
    o.qty = qty;
    link(b, i);
  }

  void link(SymbolBook& b, std::uint32_t i) {
    const Order& o = pool_[i];
    if (o.side() == Side::Buy) queues_.push_back(b.bids.get_or_create(o.price), pool_, i);
    else queues_.push_back(b.asks.get_or_create(o.price), pool_, i);
  }

  void unlink(SymbolBook& b, std::uint32_t i) {
    const Order& o = pool_[i];
    const auto take_out = [&](Level& l) { queues_.unlink(l, pool_, i); };
    const bool ok = o.side() == Side::Buy ? b.bids.update(o.price, take_out) : b.asks.update(o.price, take_out);
    if (!ok) ++stats_.level_missing;
  }

  std::vector<SymbolBook> books_;
  OrderPool pool_;
  Index index_;
  Queues queues_;
  BookStats stats_;
};

}  // namespace obl::book
