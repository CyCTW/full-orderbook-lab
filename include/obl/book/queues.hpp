#pragma once
// FIFO queue policies: how the orders resting at one price level are kept in
// time priority. L3Book is parameterized on one of these.
//
//   ListQueues    intrusive doubly linked list through Order::prev/next.
//                 Removing an order writes to both neighbours' nodes: two
//                 extra random cache lines per cancel/fill.
//   VectorQueues  per-level array of pool indices; Order::prev holds the
//                 order's slot. Removal writes a tombstone into the (usually
//                 hot) level array and never touches other orders; the array
//                 is compacted once half of it is tombstones, which rewrites
//                 the survivors' slot numbers (amortized O(1) per removal).

#include <cstdint>
#include <vector>

#include "obl/book/order_pool.hpp"

namespace obl::book {

struct ListQueues {
  static constexpr const char* name = nullptr;  // the default; not shown in variant names

  void push_back(Level& l, OrderPool& pool, std::uint32_t i) { level_push_back(l, pool, i); }
  void unlink(Level& l, OrderPool& pool, std::uint32_t i) { level_unlink(l, pool, i); }
  void release(const Level&) {}

  // Visits pool indices in queue order. f may release the visited node.
  template <class F>
  void for_each(const Level& l, const OrderPool& pool, F&& f) const {
    for (std::uint32_t i = l.head; i != kNil;) {
      const std::uint32_t next = pool[i].next;
      f(i);
      i = next;
    }
  }
};

class VectorQueues {
 public:
  static constexpr const char* name = "vector_queue";

  // Level::head holds the queue id (kNil = none yet); Order::prev the slot.
  void push_back(Level& l, OrderPool& pool, std::uint32_t i) {
    if (l.head == kNil) l.head = acquire();
    Queue& q = queues_[l.head];
    Order& o = pool[i];
    o.prev = static_cast<std::uint32_t>(q.slots.size());
    o.next = kNil;
    q.slots.push_back(i);
    l.qty += o.qty;
    ++l.count;
  }

  void unlink(Level& l, OrderPool& pool, std::uint32_t i) {
    Queue& q = queues_[l.head];
    const Order& o = pool[i];
    q.slots[o.prev] = kNil;
    ++q.dead;
    l.qty -= o.qty;
    --l.count;
    if (l.count == 0) {
      release(l);  // the level container erases the level right after this
    } else if (q.dead >= kMinCompact && q.dead * 2 >= q.slots.size()) {
      compact(q, pool);
    }
  }

  void release(const Level& l) {
    if (l.head == kNil) return;
    Queue& q = queues_[l.head];
    q.slots.clear();  // keeps capacity for the next level that reuses this queue
    q.dead = 0;
    free_.push_back(l.head);
  }

  template <class F>
  void for_each(const Level& l, const OrderPool&, F&& f) const {
    if (l.head == kNil) return;
    for (const std::uint32_t i : queues_[l.head].slots)
      if (i != kNil) f(i);
  }

 private:
  static constexpr std::size_t kMinCompact = 16;

  struct Queue {
    std::vector<std::uint32_t> slots;
    std::size_t dead = 0;
  };

  std::uint32_t acquire() {
    if (!free_.empty()) {
      const std::uint32_t id = free_.back();
      free_.pop_back();
      return id;
    }
    queues_.emplace_back();
    return static_cast<std::uint32_t>(queues_.size() - 1);
  }

  static void compact(Queue& q, OrderPool& pool) {
    std::size_t w = 0;
    for (const std::uint32_t i : q.slots) {
      if (i == kNil) continue;
      pool[i].prev = static_cast<std::uint32_t>(w);
      q.slots[w++] = i;
    }
    q.slots.resize(w);
    q.dead = 0;
  }

  std::vector<Queue> queues_;
  std::vector<std::uint32_t> free_;
};

}  // namespace obl::book
