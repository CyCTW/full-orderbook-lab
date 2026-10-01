#pragma once
// Price levels in a dense array indexed by tick: O(1) level lookup, best-price
// maintenance through an occupancy bitmap (64 ticks per word scan).
//
// The window grows on demand (both directions) up to MaxSpan ticks. Prices
// that are off-tick or would push the window beyond MaxSpan go to an overflow
// std::map, so correctness never depends on the tick size or the price range;
// only speed does. When the window later grows over prices held in overflow,
// they are migrated so every price lives in exactly one place.
//
// Recentering: when a better price arrives outside a window that cannot grow
// any further (or the window is empty), the window is moved so that price
// sits near its "better" edge: window levels go to the far store, far levels
// inside the new range come back. Rare, O(span + levels moved).
//
// MaxSpan, the far store type and the reported name are template parameters,
// so a small window + B-tree far store ("hybrid") is the same code.

#include <bit>
#include <functional>
#include <map>
#include <vector>

#include "obl/book/levels_map.hpp"
#include "obl/book/order_pool.hpp"

namespace obl::book {

inline constexpr char kDenseArrayName[] = "dense_array";

template <Side S, std::int64_t MaxSpan = (1 << 16), class Far = std::map<Price, Level, LevelCmp<S>>,
          const char* Name = kDenseArrayName>
class ArrayLevels {
  static_assert(MaxSpan % 64 == 0 && MaxSpan >= 256);

 public:
  static constexpr const char* name = Name;
  static constexpr std::int64_t kInitialSpan = 256;

  void set_tick(Price tick) {
    if (size() == 0 && tick > 0) {
      tick_ = tick;
      slots_.clear();
      bits_.clear();
    }
  }

  Level& get_or_create(Price p) {
    std::int64_t s = slot_of(p);
    if (s < 0 && on_tick(p)) {
      const std::int64_t t = p / tick_;
      if (grow_to(t) || (should_recenter(t) && recenter(t))) s = slot_of(p);
    }
    if (s < 0) {
      auto [it, inserted] = overflow_.try_emplace(p);
      if (inserted) {
        it->second.price = p;
        ++diag_.overflow_levels_created;
        if (!on_tick(p)) ++diag_.off_tick_levels;
      }
      return it->second;
    }
    Level& l = slots_[s];
    if (!test(s)) {
      l = Level{};
      l.price = p;
      set(s);
      ++window_levels_;
      if (best_ < 0 || slot_better(s, best_)) best_ = s;
    }
    return l;
  }

  template <class F>
  bool update(Price p, F&& f) {
    const std::int64_t s = slot_of(p);
    if (s >= 0) {
      if (!test(s)) return false;
      Level& l = slots_[s];
      f(l);
      if (l.count == 0) erase_slot(s);
      return true;
    }
    auto it = overflow_.find(p);
    if (it == overflow_.end()) return false;
    f(it->second);
    if (it->second.count == 0) overflow_.erase(it);
    return true;
  }

  const Level* best() const {
    const Level* w = best_ >= 0 ? &slots_[best_] : nullptr;
    const Level* o = overflow_.empty() ? nullptr : &overflow_.begin()->second;
    if (!w) return o;
    if (!o) return w;
    return better<S>(o->price, w->price) ? o : w;
  }

  template <class F>
  void for_each(F&& f) const {
    std::int64_t s = best_;
    auto it = overflow_.begin();
    while (s >= 0 || it != overflow_.end()) {
      const bool take_window =
          s >= 0 && (it == overflow_.end() || !better<S>(it->second.price, slots_[s].price));
      if (take_window) {
        if (!f(slots_[s])) return;
        s = next_worse(s);
      } else {
        if (!f(it->second)) return;
        ++it;
      }
    }
  }

  std::size_t size() const { return window_levels_ + overflow_.size(); }
  std::size_t overflow_levels() const { return overflow_.size(); }

  // Slow-path counters (diagnostics only).
  struct Diag {
    std::uint64_t grows = 0, grow_slots_copied = 0, overflow_levels_created = 0, off_tick_levels = 0, recenters = 0;
  };
  const Diag& diag() const { return diag_; }
  std::size_t window_span() const { return slots_.size(); }

  void clear() {
    slots_.clear();
    bits_.clear();
    overflow_.clear();
    best_ = -1;
    window_levels_ = 0;
  }

 private:

  bool on_tick(Price p) const { return p >= 0 && p % tick_ == 0; }

  std::int64_t span() const { return static_cast<std::int64_t>(slots_.size()); }

  // Slot index for p if p maps into the current window, else -1.
  std::int64_t slot_of(Price p) const {
    if (slots_.empty() || !on_tick(p)) return -1;
    const std::int64_t s = p / tick_ - lo_;
    return (s >= 0 && s < span()) ? s : -1;
  }

  // Higher tick index = higher price. Buy: higher is better; Sell: lower is better.
  static bool slot_better(std::int64_t a, std::int64_t b) {
    if constexpr (S == Side::Buy) return a > b;
    else return a < b;
  }

  bool test(std::int64_t s) const { return (bits_[s >> 6] >> (s & 63)) & 1u; }
  void set(std::int64_t s) { bits_[s >> 6] |= std::uint64_t(1) << (s & 63); }
  void reset(std::int64_t s) { bits_[s >> 6] &= ~(std::uint64_t(1) << (s & 63)); }

  void erase_slot(std::int64_t s) {
    reset(s);
    --window_levels_;
    if (s == best_) best_ = next_worse(s);
  }

  std::int64_t next_worse(std::int64_t s) const {
    if constexpr (S == Side::Buy) return scan_down(s - 1);
    else return scan_up(s + 1);
  }

  // Highest set bit at index <= from, or -1.
  std::int64_t scan_down(std::int64_t from) const {
    if (from < 0) return -1;
    std::int64_t w = from >> 6;
    std::uint64_t word = bits_[w] & (~std::uint64_t(0) >> (63 - (from & 63)));
    while (true) {
      if (word) return (w << 6) + 63 - std::countl_zero(word);
      if (--w < 0) return -1;
      word = bits_[w];
    }
  }

  // Lowest set bit at index >= from, or -1.
  std::int64_t scan_up(std::int64_t from) const {
    if (from >= span()) return -1;
    std::int64_t w = from >> 6;
    const std::int64_t nw = static_cast<std::int64_t>(bits_.size());
    std::uint64_t word = bits_[w] & (~std::uint64_t(0) << (from & 63));
    while (true) {
      if (word) return (w << 6) + std::countr_zero(word);
      if (++w >= nw) return -1;
      word = bits_[w];
    }
  }

  // Extends the window to cover tick index t. Returns false if that would
  // exceed MaxSpan (the price then goes to overflow).
  bool grow_to(std::int64_t t) {
    if (slots_.empty()) {
      lo_ = t - kInitialSpan / 2;
      slots_.assign(kInitialSpan, Level{});
      bits_.assign(kInitialSpan / 64, 0);
      best_ = -1;
      migrate_overflow();
      return true;
    }
    const std::int64_t hi = lo_ + span();
    std::int64_t new_lo = lo_, new_hi = hi;
    if (t < lo_) {
      new_lo = std::min(t, lo_ - span());
      new_lo = lo_ - (((lo_ - new_lo) + 63) / 64) * 64;  // keep bitmap words aligned
    } else {
      new_hi = std::max(t + 1, hi + span());
      new_hi = hi + (((new_hi - hi) + 63) / 64) * 64;
    }
    if (new_hi - new_lo > MaxSpan) {
      // clamp the doubling, but still require t to fit
      if (t < lo_) new_lo = hi - MaxSpan;
      else new_hi = lo_ + MaxSpan;
      if (t < new_lo || t >= new_hi || new_hi - new_lo <= span()) return false;
    }
    ++diag_.grows;
    diag_.grow_slots_copied += slots_.size();
    const std::int64_t front = lo_ - new_lo;  // multiple of 64
    std::vector<Level> slots(static_cast<std::size_t>(new_hi - new_lo));
    std::vector<std::uint64_t> bits(slots.size() / 64, 0);
    std::copy(slots_.begin(), slots_.end(), slots.begin() + front);
    std::copy(bits_.begin(), bits_.end(), bits.begin() + front / 64);
    slots_.swap(slots);
    bits_.swap(bits);
    lo_ = new_lo;
    if (best_ >= 0) best_ += front;
    migrate_overflow();
    return true;
  }

  // Pulls far-store levels whose prices now fall inside the window. Only the
  // window's price range of the (ordered) far store is visited.
  void migrate_overflow() {
    if (slots_.empty() || overflow_.empty()) return;
    const Price plo = lo_ * tick_, phi = (lo_ + span() - 1) * tick_;
    auto it = overflow_.lower_bound(S == Side::Buy ? phi : plo);  // first entry inside, in far-store order
    while (it != overflow_.end() && it->first >= plo && it->first <= phi) {
      const std::int64_t s = slot_of(it->first);
      if (s < 0) {  // off-tick: stays in the far store
        ++it;
        continue;
      }
      slots_[s] = it->second;
      set(s);
      ++window_levels_;
      if (best_ < 0 || slot_better(s, best_)) best_ = s;
      it = overflow_.erase(it);
    }
  }

  // A price outside a full-size window: recenter only if it would become the
  // window's best (the market moved), never for far-away worse prices.
  bool should_recenter(std::int64_t t) const {
    if (best_ < 0) return true;
    if constexpr (S == Side::Buy) return t > lo_ + best_;
    else return t < lo_ + best_;
  }

  bool recenter(std::int64_t t) {
    ++diag_.recenters;
    for (std::int64_t s = 0; s < span(); ++s) {
      if (!test(s)) continue;
      overflow_.try_emplace(slots_[s].price, slots_[s]);
    }
    std::fill(bits_.begin(), bits_.end(), 0);
    const std::int64_t margin = span() / 8;  // room for prices that improve on the best
    lo_ = S == Side::Buy ? t - (span() - margin) : t - margin;
    best_ = -1;
    window_levels_ = 0;
    migrate_overflow();
    return true;
  }

  Price tick_ = 1;
  std::int64_t lo_ = 0;  // tick index of slot 0
  std::vector<Level> slots_;
  std::vector<std::uint64_t> bits_;
  std::int64_t best_ = -1;
  std::size_t window_levels_ = 0;
  Far overflow_;
  Diag diag_;
};

}  // namespace obl::book
