#pragma once
// Price levels in a red-black tree (std::map). The textbook baseline:
// O(log n) everything, one heap node per level, pointer chasing on each lookup.

#include <functional>
#include <map>

#include "obl/book/order_pool.hpp"

namespace obl::book {

template <Side S>
class MapLevels {
 public:
  static constexpr const char* name = "std::map";

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
  using Cmp = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;
  std::map<Price, Level, Cmp> levels_;
};

}  // namespace obl::book
