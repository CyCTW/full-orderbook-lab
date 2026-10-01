#pragma once
// Price levels in an ordered associative container, best level first.
//   MapLevels      std::map (red-black tree): the textbook baseline
//   PoolMapLevels  std::map with a fixed-size node pool allocator
//   BTreeLevels    absl::btree_map (when built with abseil): many keys per node,
//                  far fewer cache lines touched per lookup than a binary tree

#include <functional>
#include <map>

#include "obl/book/node_pool.hpp"
#include "obl/book/order_pool.hpp"

#ifdef OBL_HAVE_ABSEIL
#include "absl/container/btree_map.h"
#endif

namespace obl::book {

template <Side S>
using LevelCmp = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;

template <Side S, class Map, const char* Name>
class OrderedMapLevels {
 public:
  static constexpr const char* name = Name;

  void set_tick(Price) {}

  Level& get_or_create(Price p) {
    auto [it, inserted] = levels_.try_emplace(p);
    if (inserted) it->second.price = p;
    return it->second;
  }

  // Applies f to the level at p and erases the level if it became empty.
  // Returns false if no such level exists.
  template <class F>
  bool update(Price p, F&& f) {
    auto it = levels_.find(p);
    if (it == levels_.end()) return false;
    f(it->second);
    if (it->second.count == 0) levels_.erase(it);
    return true;
  }

  const Level* best() const { return levels_.empty() ? nullptr : &levels_.begin()->second; }

  // Visits levels best -> worst until f returns false.
  template <class F>
  void for_each(F&& f) const {
    for (const auto& [p, l] : levels_)
      if (!f(l)) return;
  }

  std::size_t size() const { return levels_.size(); }
  void clear() { levels_.clear(); }

 private:
  Map levels_;
};

inline constexpr char kStdMapName[] = "std::map";
inline constexpr char kPoolMapName[] = "std::map(pool alloc)";
inline constexpr char kBTreeName[] = "absl::btree_map";

template <Side S>
using MapLevels = OrderedMapLevels<S, std::map<Price, Level, LevelCmp<S>>, kStdMapName>;

template <Side S>
using PoolMapLevels =
    OrderedMapLevels<S, std::map<Price, Level, LevelCmp<S>, NodePoolAllocator<std::pair<const Price, Level>>>,
                     kPoolMapName>;

#ifdef OBL_HAVE_ABSEIL
template <Side S>
using BTreeLevels = OrderedMapLevels<S, absl::btree_map<Price, Level, LevelCmp<S>>, kBTreeName>;
#endif

}  // namespace obl::book
