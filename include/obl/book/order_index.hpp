#pragma once
// (symbol, order id) -> pool index maps.
//
// The key includes the symbol because OMD-C order ids are only unique per
// security; XDP ids would work alone but we keep one model for both feeds.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "obl/book/order_pool.hpp"
#include "obl/common.hpp"

#ifdef OBL_HAVE_UNORDERED_DENSE
#include <ankerl/unordered_dense.h>
#endif
#ifdef OBL_HAVE_ABSEIL
#include "absl/container/flat_hash_map.h"
#endif

namespace obl::book {

inline std::uint64_t order_key_hash(SymbolId sym, OrderId id) {
  return mix64(id ^ (std::uint64_t(sym) * 0x9E3779B97F4A7C15ULL));
}

struct OrderKey {
  OrderId id;
  SymbolId sym;
  bool operator==(const OrderKey&) const = default;
};

struct OrderKeyHash {
  using is_avalanching = void;  // ankerl::unordered_dense: hash is already well mixed
  std::size_t operator()(const OrderKey& k) const { return order_key_hash(k.sym, k.id); }
};

// Adapter for any unordered_map-like container keyed by OrderKey.
template <class Map, const char* Name>
class HashMapOrderIndex {
 public:
  static constexpr const char* name = Name;

  void reserve(std::size_t n) { map_.reserve(n); }

  bool insert(SymbolId sym, OrderId id, std::uint32_t value) {
    return map_.try_emplace(OrderKey{id, sym}, value).second;
  }

  std::uint32_t find(SymbolId sym, OrderId id) const {
    auto it = map_.find(OrderKey{id, sym});
    return it == map_.end() ? kNil : it->second;
  }

  // Removes the entry and returns its value, or kNil if absent.
  std::uint32_t erase(SymbolId sym, OrderId id) {
    auto it = map_.find(OrderKey{id, sym});
    if (it == map_.end()) return kNil;
    const std::uint32_t v = it->second;
    map_.erase(it);
    return v;
  }

  void prefetch(SymbolId sym, OrderId id) const
    requires requires(const Map& m) { m.prefetch(OrderKey{}); }
  {
    map_.prefetch(OrderKey{id, sym});
  }

  std::size_t size() const { return map_.size(); }

 private:
  Map map_;
};

inline constexpr char kStdIndexName[] = "unordered_map";
using StdOrderIndex = HashMapOrderIndex<std::unordered_map<OrderKey, std::uint32_t, OrderKeyHash>, kStdIndexName>;

#ifdef OBL_HAVE_UNORDERED_DENSE
inline constexpr char kDenseIndexName[] = "ankerl::unordered_dense";
using DenseOrderIndex =
    HashMapOrderIndex<ankerl::unordered_dense::map<OrderKey, std::uint32_t, OrderKeyHash>, kDenseIndexName>;
#endif

#ifdef OBL_HAVE_ABSEIL
inline constexpr char kAbslIndexName[] = "absl::flat_hash_map";
using AbslOrderIndex = HashMapOrderIndex<absl::flat_hash_map<OrderKey, std::uint32_t, OrderKeyHash>, kAbslIndexName>;
#endif

// Linear probing, power-of-two capacity, max load 1/2, backward-shift deletion
// (no tombstones, so probe lengths do not degrade under heavy churn).
class OpenAddressingOrderIndex {
 public:
  static constexpr const char* name = "open_addressing";

  OpenAddressingOrderIndex() { rehash(1024); }

  void reserve(std::size_t n) {
    std::size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    if (cap > slots_.size()) rehash(cap);
  }

  bool insert(SymbolId sym, OrderId id, std::uint32_t value) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    std::size_t i = order_key_hash(sym, id) & mask_;
    while (slots_[i].value != kNil) {
      if (slots_[i].id == id && slots_[i].sym == sym) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = {id, sym, value};
    ++size_;
    return true;
  }

  std::uint32_t find(SymbolId sym, OrderId id) const {
    std::size_t i = order_key_hash(sym, id) & mask_;
    while (slots_[i].value != kNil) {
      if (slots_[i].id == id && slots_[i].sym == sym) return slots_[i].value;
      i = (i + 1) & mask_;
    }
    return kNil;
  }

  void prefetch(SymbolId sym, OrderId id) const {
    __builtin_prefetch(&slots_[order_key_hash(sym, id) & mask_]);
  }

  std::uint32_t erase(SymbolId sym, OrderId id) {
    std::size_t i = order_key_hash(sym, id) & mask_;
    while (true) {
      if (slots_[i].value == kNil) return kNil;
      if (slots_[i].id == id && slots_[i].sym == sym) break;
      i = (i + 1) & mask_;
    }
    const std::uint32_t v = slots_[i].value;
    // backward-shift: pull later entries of the cluster into the hole if their
    // home slot is not cyclically within (hole, j].
    std::size_t hole = i;
    std::size_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].value == kNil) break;
      const std::size_t home = order_key_hash(slots_[j].sym, slots_[j].id) & mask_;
      if (((j - home) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kNil;
    --size_;
    return v;
  }

  std::size_t size() const { return size_; }

 private:
  struct Slot {
    OrderId id;
    SymbolId sym;
    std::uint32_t value;  // kNil = empty
  };

  void rehash(std::size_t cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(cap, Slot{0, 0, kNil});
    mask_ = cap - 1;
    size_ = 0;
    for (const Slot& s : old)
      if (s.value != kNil) insert(s.sym, s.id, s.value);
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

// 8-byte slots: 32-bit fingerprint + pool index. The full key is not stored;
// a fingerprint hit is confirmed against the order node, which the caller is
// about to touch anyway, so verification costs no extra cache miss. Half the
// footprint of OpenAddressingOrderIndex => 8 slots per cache line and a table
// that is twice as likely to be cache resident.
//
// The home slot is derived from the fingerprint (fastrange on its 32 bits), so
// rehash and backward-shift deletion never need to read order nodes.
// FpBits < 32 exists only so tests can force fingerprint collisions.
template <unsigned FpBits = 32>
class CompactOrderIndexT {
  static_assert(FpBits >= 1 && FpBits <= 32);

 public:
  static constexpr const char* name = FpBits == 32 ? "compact_fp(8B)" : "compact_fp(8B, narrow fp: test only)";

  CompactOrderIndexT() { rehash(1024); }

  void attach(const OrderPool* pool) { pool_ = pool; }

  void reserve(std::size_t n) {
    std::size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    if (cap > slots_.size()) rehash(cap);
  }

  // The order node at `value` must already hold (sym, id).
  bool insert(SymbolId sym, OrderId id, std::uint32_t value) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    const std::uint32_t fp = fingerprint(sym, id);
    std::size_t i = home(fp);
    while (slots_[i].value != kNil) {
      if (slots_[i].fp == fp && matches(slots_[i].value, sym, id)) return false;
      i = (i + 1) & mask_;
    }
    slots_[i] = {fp, value};
    ++size_;
    return true;
  }

  std::uint32_t find(SymbolId sym, OrderId id) const {
    const std::uint32_t fp = fingerprint(sym, id);
    std::size_t i = home(fp);
    while (slots_[i].value != kNil) {
      if (slots_[i].fp == fp && matches(slots_[i].value, sym, id)) return slots_[i].value;
      i = (i + 1) & mask_;
    }
    return kNil;
  }

  void prefetch(SymbolId sym, OrderId id) const { __builtin_prefetch(&slots_[home(fingerprint(sym, id))]); }

  // First fingerprint match WITHOUT verifying the key (no order-node read).
  // For prefetching only: the result may be a different order, or kNil.
  std::uint32_t peek(SymbolId sym, OrderId id) const {
    const std::uint32_t fp = fingerprint(sym, id);
    std::size_t i = home(fp);
    while (slots_[i].value != kNil) {
      if (slots_[i].fp == fp) return slots_[i].value;
      i = (i + 1) & mask_;
    }
    return kNil;
  }

  std::uint32_t erase(SymbolId sym, OrderId id) {
    const std::uint32_t fp = fingerprint(sym, id);
    std::size_t i = home(fp);
    while (true) {
      if (slots_[i].value == kNil) return kNil;
      if (slots_[i].fp == fp && matches(slots_[i].value, sym, id)) break;
      i = (i + 1) & mask_;
    }
    const std::uint32_t v = slots_[i].value;
    std::size_t hole = i, j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].value == kNil) break;
      const std::size_t h = home(slots_[j].fp);
      if (((j - h) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kNil;
    --size_;
    return v;
  }

  std::size_t size() const { return size_; }

 private:
  struct Slot {
    std::uint32_t fp;
    std::uint32_t value;  // kNil = empty
  };

  static std::uint32_t fingerprint(SymbolId sym, OrderId id) {
    // keep the top FpBits bits (fastrange in home() uses the high bits)
    const auto h = static_cast<std::uint32_t>(order_key_hash(sym, id) >> 32);
    return FpBits == 32 ? h : h & ~(0xFFFFFFFFu >> FpBits);
  }
  std::size_t home(std::uint32_t fp) const { return static_cast<std::size_t>((std::uint64_t(fp) * slots_.size()) >> 32); }

  bool matches(std::uint32_t v, SymbolId sym, OrderId id) const {
    const Order& o = (*pool_)[v];
    return o.id == id && o.symbol() == sym;
  }

  void rehash(std::size_t cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(cap, Slot{0, kNil});
    mask_ = cap - 1;
    for (const Slot& s : old) {
      if (s.value == kNil) continue;
      std::size_t i = home(s.fp);
      while (slots_[i].value != kNil) i = (i + 1) & mask_;
      slots_[i] = s;
    }
  }

  const OrderPool* pool_ = nullptr;
  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

using CompactOrderIndex = CompactOrderIndexT<32>;

}  // namespace obl::book
