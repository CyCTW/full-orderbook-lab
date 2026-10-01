#pragma once
// Price levels in a contiguous sorted vector, best level at the BACK.
// Most activity happens near the touch, so inserts/erases near the back only
// shift a few elements, and a linear scan from the back usually terminates
// within a handful of cache lines. The binary-search flavour is kept to show
// when the linear scan stops paying off (deep, wide books).

#include <algorithm>
#include <vector>

#include "obl/book/order_pool.hpp"

namespace obl::book {

enum class VecSearch { Linear, Binary };

template <Side S, VecSearch Search>
class SortedVectorLevels {
 public:
  static constexpr const char* name =
      Search == VecSearch::Linear ? "sorted_vector(linear)" : "sorted_vector(binary)";

  void set_tick(Price) {}

  Level& get_or_create(Price p) {
    const std::size_t i = lower(p);  // first index whose price is not worse than p
    if (i < levels_.size() && levels_[i].price == p) return levels_[i];
    Level& l = *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Level{});
    l.price = p;
    return l;
  }

  template <class F>
  bool update(Price p, F&& f) {
    const std::size_t i = lower(p);
    if (i == levels_.size() || levels_[i].price != p) return false;
    f(levels_[i]);
    if (levels_[i].count == 0) levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }

  const Level* best() const { return levels_.empty() ? nullptr : &levels_.back(); }

  template <class F>
  void for_each(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it)
      if (!f(*it)) return;
  }

  std::size_t size() const { return levels_.size(); }
  void clear() { levels_.clear(); }

 private:
  // Ordering worst..best: index i < j  <=>  levels_[i] is worse than levels_[j].
  std::size_t lower(Price p) const {
    if constexpr (Search == VecSearch::Linear) {
      std::size_t i = levels_.size();
      while (i > 0 && better<S>(levels_[i - 1].price, p)) --i;
      // levels_[i-1] (if any) is not better than p; step onto an exact match
      if (i > 0 && levels_[i - 1].price == p) --i;
      return i;
    } else {
      auto it = std::lower_bound(levels_.begin(), levels_.end(), p,
                                 [](const Level& l, Price v) { return better<S>(v, l.price); });
      return static_cast<std::size_t>(it - levels_.begin());
    }
  }

  std::vector<Level> levels_;
};

}  // namespace obl::book
