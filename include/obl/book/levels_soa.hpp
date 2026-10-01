#pragma once
// Sorted vector of levels in struct-of-arrays form, best level at the BACK.
// The search touches only the dense price array (8 levels per cache line
// instead of 2 for the array of Level structs), so a linear scan from the back
// covers ~4x more levels per cache line and is easy for the compiler to vectorize.

#include <vector>

#include "obl/book/order_pool.hpp"

namespace obl::book {

template <Side S>
class SoaVectorLevels {
 public:
  static constexpr const char* name = "sorted_vector_soa(linear)";

  void set_tick(Price) {}

  Level& get_or_create(Price p) {
    const std::size_t i = lower(p);
    if (i < prices_.size() && prices_[i] == p) return levels_[i];
    prices_.insert(prices_.begin() + static_cast<std::ptrdiff_t>(i), p);
    Level& l = *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Level{});
    l.price = p;
    return l;
  }

  template <class F>
  bool update(Price p, F&& f) {
    const std::size_t i = lower(p);
    if (i == prices_.size() || prices_[i] != p) return false;
    f(levels_[i]);
    if (levels_[i].count == 0) {
      prices_.erase(prices_.begin() + static_cast<std::ptrdiff_t>(i));
      levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return true;
  }

  const Level* best() const { return levels_.empty() ? nullptr : &levels_.back(); }

  template <class F>
  void for_each(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it)
      if (!f(*it)) return;
  }

  std::size_t size() const { return levels_.size(); }
  void clear() {
    prices_.clear();
    levels_.clear();
  }

 private:
  // First index whose price is not worse than p (ordering worst..best).
  std::size_t lower(Price p) const {
    std::size_t i = prices_.size();
    const Price* d = prices_.data();
    while (i > 0 && better<S>(d[i - 1], p)) --i;
    if (i > 0 && d[i - 1] == p) --i;
    return i;
  }

  std::vector<Price> prices_;
  std::vector<Level> levels_;
};

}  // namespace obl::book
