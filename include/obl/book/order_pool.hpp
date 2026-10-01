#pragma once
// Order storage shared by every book variant: a slab of fixed-size Order nodes
// addressed by 32-bit index, with an intrusive doubly linked FIFO per price level.
// Indices (not pointers) are used so the slab and level containers may reallocate.

#include <cstdint>
#include <vector>

#include "obl/common.hpp"

namespace obl::book {

struct Order {
  OrderId id;
  Price price;
  Qty qty;
  SymbolId symbol;
  std::uint32_t prev;
  std::uint32_t next;
  Side side;
};

struct Level {
  Price price = 0;
  std::uint64_t qty = 0;      // aggregate visible quantity
  std::uint32_t count = 0;    // number of orders
  std::uint32_t head = kNil;  // oldest order (front of queue)
  std::uint32_t tail = kNil;  // newest order
};

class OrderPool {
 public:
  void reserve(std::size_t n) { slots_.reserve(n); }

  std::uint32_t alloc() {
    ++live_;
    if (free_ != kNil) {
      const std::uint32_t i = free_;
      free_ = slots_[i].next;
      return i;
    }
    slots_.emplace_back();
    return static_cast<std::uint32_t>(slots_.size() - 1);
  }

  void release(std::uint32_t i) {
    slots_[i].next = free_;
    free_ = i;
    --live_;
  }

  Order& operator[](std::uint32_t i) { return slots_[i]; }
  const Order& operator[](std::uint32_t i) const { return slots_[i]; }
  std::size_t live() const { return live_; }
  std::size_t capacity() const { return slots_.size(); }

 private:
  std::vector<Order> slots_;
  std::uint32_t free_ = kNil;
  std::size_t live_ = 0;
};

inline void level_push_back(Level& l, OrderPool& pool, std::uint32_t i) {
  Order& o = pool[i];
  o.prev = l.tail;
  o.next = kNil;
  if (l.tail != kNil) pool[l.tail].next = i;
  else l.head = i;
  l.tail = i;
  l.qty += o.qty;
  ++l.count;
}

inline void level_unlink(Level& l, OrderPool& pool, std::uint32_t i) {
  Order& o = pool[i];
  if (o.prev != kNil) pool[o.prev].next = o.next;
  else l.head = o.next;
  if (o.next != kNil) pool[o.next].prev = o.prev;
  else l.tail = o.prev;
  l.qty -= o.qty;
  --l.count;
}

}  // namespace obl::book
