#pragma once
// Aggregate-only book with the order data stored inside the hash table.
//
// A book that publishes price levels (total quantity and order count) does
// not need queue position, so no level ever points at an order and orders
// need no stable address. The (symbol, id) -> order lookup can then return
// the order itself: one random memory access per message instead of two
// (index slot, then order node), and nothing else to touch.
//
//   slot: {id, price, qty, symbol<<1|side}   32 B (24 B used), two per cache line
//
// Linear probing, power-of-two capacity, max load 1/2, backward-shift
// deletion. Same public surface as L3Book minus per-order queue queries.

#include <cstdint>
#include <string>
#include <vector>

#include "obl/book/l3_book.hpp"

namespace obl::book {

template <template <Side> class LevelsT>
class AggBook {
 public:
  using Bids = LevelsT<Side::Buy>;
  using Asks = LevelsT<Side::Sell>;

  struct SymbolBook {
    Bids bids;
    Asks asks;
  };

  // Order fields as an Order-like view (what find_order() callers read).
  struct alignas(32) Slot {
    OrderId id;
    Price price;
    Qty qty;
    std::uint32_t sym_side;  // kEmpty = free

    Side side() const { return static_cast<Side>(sym_side & 1u); }
    SymbolId symbol() const { return sym_side >> 1; }
  };
  static_assert(sizeof(Slot) == 32);

  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
  static constexpr SymbolId kMaxSymbol = 0x7FFFFFFE;  // keeps sym_side != kEmpty
  static constexpr bool kQueueOrder = false;
  static constexpr bool kCanPrefetch = true;

  static std::string name() { return std::string(Bids::name) + " + inline_table(aggregate)"; }

  AggBook() { rehash(1024); }
  AggBook(const AggBook&) = delete;
  AggBook& operator=(const AggBook&) = delete;

  void reserve(std::size_t orders, std::size_t symbols) {
    std::size_t cap = 16;
    while (cap < orders * 2) cap <<= 1;
    if (cap > slots_.size()) rehash(cap);
    books_.reserve(symbols);
  }

  void set_tick(SymbolId sym, Price tick) {
    SymbolBook& b = book_for(sym);
    b.bids.set_tick(tick);
    b.asks.set_tick(tick);
  }

  bool add(SymbolId sym, OrderId id, Side side, Price price, Qty qty) {
    if (sym > kMaxSymbol) {
      ++stats_.bad_symbol;
      return false;
    }
    if (!insert({id, price, qty, pack(sym, side)})) {
      ++stats_.duplicate_add;
      return false;
    }
    level_add(book_for(sym), side, price, qty);
    return true;
  }

  bool remove(SymbolId sym, OrderId id) {
    const std::size_t i = find_slot(sym, id);
    if (i == kNotFound) {
      ++stats_.unknown_order;
      return false;
    }
    const Slot o = slots_[i];
    erase_at(i);
    level_sub(books_[sym], o.side(), o.price, o.qty, 1);
    return true;
  }

  bool execute(SymbolId sym, OrderId id, Qty executed) {
    const std::size_t i = find_slot(sym, id);
    if (i == kNotFound) {
      ++stats_.unknown_order;
      return false;
    }
    Slot& o = slots_[i];
    if (executed >= o.qty) {
      if (executed > o.qty) ++stats_.over_execution;
      const Slot gone = o;
      erase_at(i);
      level_sub(books_[sym], gone.side(), gone.price, gone.qty, 1);
      return true;
    }
    o.qty -= executed;
    level_sub(books_[sym], o.side(), o.price, executed, 0);
    return true;
  }

  // Queue priority does not exist here, so keep_priority is irrelevant.
  bool modify(SymbolId sym, OrderId id, Price price, Qty qty, bool /*keep_priority*/) {
    const std::size_t i = find_slot(sym, id);
    if (i == kNotFound) {
      ++stats_.unknown_order;
      return false;
    }
    if (qty == 0) return remove(sym, id);
    move(books_[sym], slots_[i], price, qty);
    return true;
  }

  bool replace(SymbolId sym, OrderId old_id, OrderId new_id, Side side, Price price, Qty qty) {
    return replace_impl(sym, old_id, new_id, &side, price, qty);
  }
  bool replace(SymbolId sym, OrderId old_id, OrderId new_id, Price price, Qty qty) {
    return replace_impl(sym, old_id, new_id, nullptr, price, qty);
  }

  void clear_symbol(SymbolId sym) {
    if (sym >= books_.size()) return;
    // Rebuild the table without the symbol (symbol clears are rare).
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.size(), empty_slot());
    size_ = 0;
    for (const Slot& s : old)
      if (s.sym_side != kEmpty && s.symbol() != sym) insert(s);
    books_[sym].bids.clear();
    books_[sym].asks.clear();
  }

  // One memory access per lookup, so a single prefetch stage covers it.
  void prefetch_index(SymbolId sym, OrderId id) const { __builtin_prefetch(&slots_[home(sym, id)]); }
  void prefetch_order(SymbolId, OrderId) const {}

  const Slot* find_order(SymbolId sym, OrderId id) const {
    const std::size_t i = find_slot(sym, id);
    return i == kNotFound ? nullptr : &slots_[i];
  }

  const SymbolBook* book(SymbolId sym) const { return sym < books_.size() ? &books_[sym] : nullptr; }
  std::size_t symbols() const { return books_.size(); }
  std::size_t order_count() const { return size_; }
  const BookStats& stats() const { return stats_; }

  std::vector<LevelView> depth(SymbolId sym, Side side, std::size_t n) const {
    std::vector<LevelView> out;
    const SymbolBook* b = book(sym);
    if (!b || n == 0) return out;
    const auto take = [&](const Level& l) {
      out.push_back({l.price, l.qty, l.count});
      return out.size() < n;
    };
    if (side == Side::Buy) b->bids.for_each(take);
    else b->asks.for_each(take);
    return out;
  }

  std::uint64_t state_checksum() const {
    std::uint64_t orders = 0;
    for (const Slot& s : slots_)
      if (s.sym_side != kEmpty) orders += order_hash(s.symbol(), s.id, s.side(), s.price, s.qty);
    return mix64(levels_checksum(books_) ^ orders);
  }
  std::uint64_t checksum() const { return state_checksum(); }

 private:
  static constexpr std::size_t kNotFound = ~std::size_t{0};

  static std::uint32_t pack(SymbolId sym, Side side) { return (sym << 1) | static_cast<std::uint32_t>(side); }
  static Slot empty_slot() { return Slot{0, 0, 0, kEmpty}; }
  std::size_t home(SymbolId sym, OrderId id) const { return order_key_hash(sym, id) & mask_; }

  std::size_t find_slot(SymbolId sym, OrderId id) const {
    std::size_t i = home(sym, id);
    while (slots_[i].sym_side != kEmpty) {
      if (slots_[i].id == id && slots_[i].symbol() == sym) return i;
      i = (i + 1) & mask_;
    }
    return kNotFound;
  }

  bool insert(const Slot& s) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    std::size_t i = home(s.symbol(), s.id);
    while (slots_[i].sym_side != kEmpty) {
      if (slots_[i].id == s.id && slots_[i].symbol() == s.symbol()) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = s;
    ++size_;
    return true;
  }

  void erase_at(std::size_t hole) {
    std::size_t j = hole;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].sym_side == kEmpty) break;
      const std::size_t h = home(slots_[j].symbol(), slots_[j].id);
      if (((j - h) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].sym_side = kEmpty;
    --size_;
  }

  void rehash(std::size_t cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(cap, empty_slot());
    mask_ = cap - 1;
    size_ = 0;
    for (const Slot& s : old)
      if (s.sym_side != kEmpty) insert(s);
  }

  SymbolBook& book_for(SymbolId sym) {
    if (sym >= books_.size()) books_.resize(static_cast<std::size_t>(sym) + 1);
    return books_[sym];
  }

  void level_add(SymbolBook& b, Side side, Price price, Qty qty) {
    Level& l = side == Side::Buy ? b.bids.get_or_create(price) : b.asks.get_or_create(price);
    l.qty += qty;
    ++l.count;
  }

  // Takes qty (and `orders` orders) off a level; the container drops it at count 0.
  void level_sub(SymbolBook& b, Side side, Price price, Qty qty, std::uint32_t orders) {
    const auto take = [&](Level& l) {
      l.qty -= qty;
      l.count -= orders;
    };
    const bool ok = side == Side::Buy ? b.bids.update(price, take) : b.asks.update(price, take);
    if (!ok) ++stats_.level_missing;
  }

  // Order s now rests at (price, qty) on its side.
  void move(SymbolBook& b, Slot& s, Price price, Qty qty) {
    if (price == s.price) {
      const Qty old = s.qty;
      const auto resize = [&](Level& l) { l.qty = l.qty - old + qty; };
      const bool ok = s.side() == Side::Buy ? b.bids.update(price, resize) : b.asks.update(price, resize);
      if (!ok) ++stats_.level_missing;
    } else {
      level_sub(b, s.side(), s.price, s.qty, 1);
      level_add(b, s.side(), price, qty);
    }
    s.price = price;
    s.qty = qty;
  }

  bool replace_impl(SymbolId sym, OrderId old_id, OrderId new_id, const Side* side, Price price, Qty qty) {
    const std::size_t i = find_slot(sym, old_id);
    if (i == kNotFound) {
      ++stats_.unknown_order;
      if (side) add(sym, new_id, *side, price, qty);
      return false;
    }
    const Slot old = slots_[i];
    erase_at(i);
    SymbolBook& b = books_[sym];
    const Side s = side ? *side : old.side();
    if (s != old.side()) {
      level_sub(b, old.side(), old.price, old.qty, 1);
      return add(sym, new_id, s, price, qty);
    }
    if (!insert({new_id, old.price, old.qty, old.sym_side})) {
      ++stats_.duplicate_add;
      level_sub(b, old.side(), old.price, old.qty, 1);
      return false;
    }
    // Re-find: the insert may have rehashed.
    move(b, slots_[find_slot(sym, new_id)], price, qty);
    return true;
  }

  std::vector<SymbolBook> books_;
  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
  BookStats stats_;
};

}  // namespace obl::book
