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

namespace obl::book {

struct BookStats {
  std::uint64_t unknown_order = 0;   // modify/execute/delete for an id we do not hold
  std::uint64_t duplicate_add = 0;   // add with an id that is already live
  std::uint64_t over_execution = 0;  // executed quantity exceeded remaining quantity
  std::uint64_t level_missing = 0;   // internal inconsistency; must stay 0
};

struct LevelView {
  Price price;
  std::uint64_t qty;
  std::uint32_t count;
};

template <template <Side> class LevelsT, class Index>
class L3Book {
 public:
  using Bids = LevelsT<Side::Buy>;
  using Asks = LevelsT<Side::Sell>;

  struct SymbolBook {
    Bids bids;
    Asks asks;
  };

  static std::string name() { return std::string(Bids::name) + " + " + Index::name; }

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
    const std::uint32_t i = pool_.alloc();
    if (!index_.insert(sym, id, i)) {
      pool_.release(i);
      ++stats_.duplicate_add;
      return false;
    }
    pool_[i] = Order{id, price, qty, sym, kNil, kNil, side};
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
    const bool ok = o.side == Side::Buy ? b.bids.update(o.price, reduce) : b.asks.update(o.price, reduce);
    if (!ok) ++stats_.level_missing;
    o.qty -= executed;
    return true;
  }

  // keep_priority: the exchange says the order kept its queue position
  // (XDP PositionChange == 0). A price change always loses priority.
  bool modify(SymbolId sym, OrderId id, Price price, Qty qty, bool keep_priority) {
    const std::uint32_t i = index_.find(sym, id);
    if (i == kNil) {
      ++stats_.unknown_order;
      return false;
    }
    if (qty == 0) return remove(sym, id);
    SymbolBook& b = books_[sym];
    Order& o = pool_[i];
    if (price == o.price && keep_priority) {
      const Qty old = o.qty;
      const auto resize = [&](Level& l) { l.qty = l.qty - old + qty; };
      const bool ok = o.side == Side::Buy ? b.bids.update(price, resize) : b.asks.update(price, resize);
      if (!ok) ++stats_.level_missing;
      o.qty = qty;
      return true;
    }
    unlink(b, i);
    o.price = price;
    o.qty = qty;
    link(b, i);
    return true;
  }

  bool replace(SymbolId sym, OrderId old_id, OrderId new_id, Side side, Price price, Qty qty) {
    const bool removed = remove(sym, old_id);
    return add(sym, new_id, side, price, qty) && removed;
  }

  void clear_symbol(SymbolId sym) {
    if (sym >= books_.size()) return;
    SymbolBook& b = books_[sym];
    const auto drop = [&](const Level& l) {
      for (std::uint32_t i = l.head; i != kNil;) {
        const std::uint32_t next = pool_[i].next;
        index_.erase(sym, pool_[i].id);
        pool_.release(i);
        i = next;
      }
      return true;
    };
    b.bids.for_each(drop);
    b.asks.for_each(drop);
    b.bids.clear();
    b.asks.clear();
  }

  // ------------------------------------------------------------ queries
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

  // Order ids at a level in queue (FIFO) order.
  std::vector<OrderId> queue(const Level& l) const {
    std::vector<OrderId> ids;
    for (std::uint32_t i = l.head; i != kNil; i = pool_[i].next) ids.push_back(pool_[i].id);
    return ids;
  }

  // Hash over every symbol's full depth including per-level queue order.
  // Two variants processing the same feed must produce the same value.
  std::uint64_t checksum() const {
    std::uint64_t h = 0x12345678;
    const auto mix = [&](std::uint64_t v) { h = mix64(h ^ v) + 0x9E3779B97F4A7C15ULL; };
    for (SymbolId s = 0; s < books_.size(); ++s) {
      const auto visit = [&](const Level& l) {
        mix(s);
        mix(static_cast<std::uint64_t>(l.price));
        mix(l.qty);
        mix(l.count);
        for (std::uint32_t i = l.head; i != kNil; i = pool_[i].next) {
          mix(pool_[i].id);
          mix(pool_[i].qty);
        }
        return true;
      };
      mix(0xB1D);
      books_[s].bids.for_each(visit);
      mix(0xA5C);
      books_[s].asks.for_each(visit);
    }
    return h;
  }

 private:
  SymbolBook& book_for(SymbolId sym) {
    if (sym >= books_.size()) books_.resize(static_cast<std::size_t>(sym) + 1);
    return books_[sym];
  }

  void link(SymbolBook& b, std::uint32_t i) {
    const Order& o = pool_[i];
    if (o.side == Side::Buy) level_push_back(b.bids.get_or_create(o.price), pool_, i);
    else level_push_back(b.asks.get_or_create(o.price), pool_, i);
  }

  void unlink(SymbolBook& b, std::uint32_t i) {
    const Order& o = pool_[i];
    const auto take_out = [&](Level& l) { level_unlink(l, pool_, i); };
    const bool ok = o.side == Side::Buy ? b.bids.update(o.price, take_out) : b.asks.update(o.price, take_out);
    if (!ok) ++stats_.level_missing;
  }

  std::vector<SymbolBook> books_;
  OrderPool pool_;
  Index index_;
  BookStats stats_;
};

}  // namespace obl::book
