#pragma once
// (symbol, order id) -> pool index maps.
//
// The key includes the symbol because OMD-C order ids are only unique per
// security; XDP ids would work alone but we keep one model for both feeds.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "obl/common.hpp"

namespace obl::book {

inline std::uint64_t order_key_hash(SymbolId sym, OrderId id) {
  return mix64(id ^ (std::uint64_t(sym) * 0x9E3779B97F4A7C15ULL));
}

class StdOrderIndex {
 public:
  static constexpr const char* name = "unordered_map";

  void reserve(std::size_t n) { map_.reserve(n); }

  bool insert(SymbolId sym, OrderId id, std::uint32_t value) {
    return map_.try_emplace(Key{id, sym}, value).second;
  }

  std::uint32_t find(SymbolId sym, OrderId id) const {
    auto it = map_.find(Key{id, sym});
    return it == map_.end() ? kNil : it->second;
  }

  // Removes the entry and returns its value, or kNil if absent.
  std::uint32_t erase(SymbolId sym, OrderId id) {
    auto it = map_.find(Key{id, sym});
    if (it == map_.end()) return kNil;
    const std::uint32_t v = it->second;
    map_.erase(it);
    return v;
  }

  std::size_t size() const { return map_.size(); }

 private:
  struct Key {
    OrderId id;
    SymbolId sym;
    bool operator==(const Key&) const = default;
  };
  struct Hash {
    std::size_t operator()(const Key& k) const { return order_key_hash(k.sym, k.id); }
  };
  std::unordered_map<Key, std::uint32_t, Hash> map_;
};

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

}  // namespace obl::book
