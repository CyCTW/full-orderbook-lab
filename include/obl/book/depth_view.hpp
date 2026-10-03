#pragma once
// "View first": publish top-of-book depth before the L3 book is updated.
//
// For each packet:
//   1. fast path, per message in order: work out the message's effect on the
//      symbol's cached top-N levels (Add: straight from the message; others:
//      look the order up — index slot + order node — through a per-packet
//      overlay so repeated references to one order inside the packet see
//      the earlier changes), update the cache, publish the top-5 snapshot if
//      it changed.
//   2. deferred: apply the packet's events to the L3 book as usual (the
//      lookups from step 1 left their cache lines hot).
//      If a cache can no longer tell the top 5 (it lost levels and the book
//      may hold deeper ones), it is refilled on the spot: during the fast
//      path the L3 book still holds the state from before this packet, so
//      the levels beyond the cache are exactly "pre-packet L3 level + this
//      packet's overlay deltas at that price". Every message therefore gets
//      its exact top-5 immediately; nothing is skipped or delayed.
//   3. fallback (should never run): a cache still unresolved after step 1 is
//      rebuilt from the updated L3 book and its messages are completed then.
//
// Invariant (checked by tests): after step 3 each cache holds exactly the
// best `n` levels of the L3 book side, and if it is not `truncated` the book
// side has exactly `n` levels.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "obl/book/events.hpp"
#include "obl/book/order_pool.hpp"

namespace obl::book {

struct DepthEntry {
  Price price = 0;
  std::uint64_t qty = 0;
  std::uint32_t count = 0;
  bool operator==(const DepthEntry& o) const { return price == o.price && qty == o.qty && count == o.count; }
};

inline constexpr int kPublishDepth = 5;

struct TopSnapshot {
  DepthEntry bid[kPublishDepth], ask[kPublishDepth];
  std::uint8_t nb = 0, na = 0;
  bool operator==(const TopSnapshot& o) const {
    if (nb != o.nb || na != o.na) return false;
    for (int i = 0; i < nb; ++i)
      if (!(bid[i] == o.bid[i])) return false;
    for (int i = 0; i < na; ++i)
      if (!(ask[i] == o.ask[i])) return false;
    return true;
  }
};

// Top-5 straight from an L3 book (what a conventional handler publishes).
template <class Book>
void l3_top(const Book& b, SymbolId s, TopSnapshot& t) {
  t.nb = t.na = 0;
  const auto* sb = b.book(s);
  if (!sb) return;
  sb->bids.for_each([&](const Level& l) {
    t.bid[t.nb++] = {l.price, l.qty, l.count};
    return t.nb < kPublishDepth;
  });
  sb->asks.for_each([&](const Level& l) {
    t.ask[t.na++] = {l.price, l.qty, l.count};
    return t.na < kPublishDepth;
  });
}

// Cached best levels of one book side, best first.
template <Side S, int N = 10>
struct SideView {
  DepthEntry e[N];
  int n = 0;
  bool truncated = false;  // the book side may hold levels worse than e[n-1]

  // Exact top-k known?
  bool knows_top(int k) const { return n >= k || !truncated; }

  void add(Price p, Qty q) {
    int i = 0;
    while (i < n && better<S>(e[i].price, p)) ++i;
    if (i < n && e[i].price == p) {
      e[i].qty += q;
      ++e[i].count;
      return;
    }
    if (i == n) {  // worse than everything cached
      if (n < N && !truncated) e[n++] = {p, q, 1};
      else truncated = true;  // beyond the cached range (or cache full)
      return;
    }
    if (n == N) {
      truncated = true;  // last entry falls out
      --n;
    }
    for (int j = n; j > i; --j) e[j] = e[j - 1];
    e[i] = {p, q, 1};
    ++n;
  }

  // Removes `q` from the level at `p`; `gone` = the order left the book.
  // Returns false if `p` is not cached (it must then lie beyond the cache).
  bool reduce(Price p, Qty q, bool gone) {
    for (int i = 0; i < n; ++i) {
      if (e[i].price != p) continue;
      e[i].qty -= q;
      if (gone) --e[i].count;
      if (e[i].count == 0) {
        for (int j = i; j + 1 < n; ++j) e[j] = e[j + 1];
        --n;
      }
      return true;
    }
    return false;
  }

  template <class Levels>
  void rebuild(const Levels& lv) {
    n = 0;
    truncated = false;
    lv.for_each([&](const Level& l) {
      if (n == N) {
        truncated = true;
        return false;
      }
      e[n++] = {l.price, l.qty, l.count};
      return true;
    });
  }

  void copy_top(DepthEntry* out, std::uint8_t& cnt) const {
    cnt = static_cast<std::uint8_t>(std::min(n, kPublishDepth));
    for (int i = 0; i < cnt; ++i) out[i] = e[i];
  }
};

// CacheDepth: levels kept per side. Deeper caches survive bigger sweeps
// without a refill (which has to wait for the deferred L3 update).
template <class Book, int CacheDepth = 10>
class ViewFirstEngine {
 public:
  struct Stats {
    std::uint64_t messages = 0, lookups = 0, overlay_hits = 0;
    std::uint64_t refills = 0;             // immediate refills (pre-packet L3 + overlay)
    std::uint64_t fallback_refills = 0;    // must stay 0
    std::uint64_t deferred_publishes = 0;  // messages decided after the L3 update; must stay 0 without conflation
  };

  explicit ViewFirstEngine(Book& b) : book_(b) {}

  // Processes one packet of order events. `prefetch_k` > 0 enables the rolling
  // two-stage prefetch on the fast path. `conflate`: publish once per symbol
  // after the fast path instead of per message.
  //   on_publish(sym, const TopSnapshot&)   called for every changed snapshot
  //   on_done(i)                            message i's publish decision is final
  template <class OnPublish, class OnDone>
  void process(const Event* ev, std::uint32_t n, std::size_t prefetch_k, bool conflate, OnPublish&& on_publish,
               OnDone&& on_done) {
    overlay_.clear();
    touched_.clear();
    waiting_.clear();
    const std::size_t k = prefetch_k;
    if (k)
      for (std::uint32_t i = 0; i < std::min<std::size_t>(n, 2 * k); ++i) book_.prefetch_index(ev[i].sym, ev[i].id);

    // 1. fast path
    for (std::uint32_t i = 0; i < n; ++i) {
      if (k) {
        if (i + 2 * k < n) book_.prefetch_index(ev[i + 2 * k].sym, ev[i + 2 * k].id);
        if (i + k < n && references_order(ev[i + k])) book_.prefetch_order(ev[i + k].sym, ev[i + k].id);
      }
      const Event& e = ev[i];
      ++stats_.messages;
      SymState& st = state(e.sym);
      touch(e.sym);
      fast_apply(e, st);
      refill_if_needed(e.sym, st);
      if (conflate) continue;
      if (!st.bids.knows_top(kPublishDepth) || !st.asks.knows_top(kPublishDepth)) {
        waiting_.push_back(i);  // decided after the refill
        continue;
      }
      publish_if_changed(e.sym, st, on_publish);
      on_done(i);
    }
    if (conflate) {
      for (const SymbolId s : touched_) {
        SymState& st = states_[s];
        if (!st.bids.knows_top(kPublishDepth) || !st.asks.knows_top(kPublishDepth)) continue;
        publish_if_changed(s, st, on_publish);
        st.decided = true;
      }
      for (std::uint32_t i = 0; i < n; ++i) {
        if (states_[ev[i].sym].decided) on_done(i);
        else waiting_.push_back(i);
      }
    }

    // 2. deferred L3 update
    for (std::uint32_t i = 0; i < n; ++i) apply(book_, ev[i]);

    // 3. refill caches that lost track of their top 5, then finish waiting messages
    for (const SymbolId s : touched_) {
      SymState& st = states_[s];
      const auto* sb = book_.book(s);
      bool refilled = false;
      if (!st.bids.knows_top(kPublishDepth)) st.bids.rebuild(sb->bids), refilled = true;
      if (!st.asks.knows_top(kPublishDepth)) st.asks.rebuild(sb->asks), refilled = true;
      if (refilled) {
        ++stats_.fallback_refills;
        publish_if_changed(s, st, on_publish);
      }
      st.decided = false;
      st.cleared = false;
    }
    for (const std::uint32_t i : waiting_) {
      ++stats_.deferred_publishes;
      on_done(i);
    }
  }

  // Non-order events (SetTick, ClearSymbol): apply directly, rebuild caches.
  void apply_admin(const Event& e) {
    apply(book_, e);
    if (const auto* sb = book_.book(e.sym)) {
      SymState& st = state(e.sym);
      st.bids.rebuild(sb->bids);
      st.asks.rebuild(sb->asks);
    }
  }

  // Test hook: cache contents must match the L3 book exactly.
  bool consistent(SymbolId s) const {
    if (s >= states_.size()) return true;
    const SymState& st = states_[s];
    const auto* sb = book_.book(s);
    if (!sb) return st.bids.n == 0 && st.asks.n == 0;
    return side_ok(st.bids, sb->bids) && side_ok(st.asks, sb->asks);
  }

  const Stats& stats() const { return stats_; }

 private:
  struct SymState {
    SideView<Side::Buy, CacheDepth> bids;
    SideView<Side::Sell, CacheDepth> asks;
    TopSnapshot last;  // last published
    bool decided = false;
    bool cleared = false;  // ClearSymbol seen in the current packet
  };
  struct OverlayEntry {
    SymbolId sym;
    OrderId id;
    Side side;
    Price price;  // current
    Qty qty;      // current
    bool alive;
    bool from_l3;      // was resting in the L3 book before this packet
    Price orig_price;  // its L3 price / quantity (valid if from_l3)
    Qty orig_qty;
  };
  struct Delta {
    Price price;
    std::int64_t qty;
    std::int32_t count;
  };

  SymState& state(SymbolId s) {
    if (s >= states_.size()) states_.resize(static_cast<std::size_t>(s) + 1);
    return states_[s];
  }
  void touch(SymbolId s) {
    if (std::find(touched_.begin(), touched_.end(), s) == touched_.end()) touched_.push_back(s);
  }

  // Current state of an order: this packet's overlay first, then the L3 node.
  OverlayEntry* lookup(SymbolId sym, OrderId id) {
    for (auto& o : overlay_)
      if (o.id == id && o.sym == sym) {
        ++stats_.overlay_hits;
        return &o;
      }
    ++stats_.lookups;
    const auto* o = book_.find_order(sym, id);
    if (!o) return nullptr;
    overlay_.push_back({sym, id, o->side(), o->price, o->qty, true, true, o->price, o->qty});
    return &overlay_.back();
  }

  void view_add(SymState& st, Side side, Price p, Qty q) {
    if (side == Side::Buy) st.bids.add(p, q);
    else st.asks.add(p, q);
  }
  void view_reduce(SymState& st, Side side, Price p, Qty q, bool gone) {
    if (side == Side::Buy) st.bids.reduce(p, q, gone);
    else st.asks.reduce(p, q, gone);
  }

  void fast_apply(const Event& e, SymState& st) {
    switch (e.type) {
      case EventType::Add:
        view_add(st, e.side, e.price, e.qty);
        overlay_.push_back({e.sym, e.id, e.side, e.price, e.qty, true, false, 0, 0});
        break;
      case EventType::Execute:
      case EventType::Cancel: {
        OverlayEntry* o = lookup(e.sym, e.id);
        if (!o || !o->alive) break;
        const bool gone = e.qty >= o->qty;
        const Qty q = gone ? o->qty : e.qty;
        view_reduce(st, o->side, o->price, q, gone);
        o->qty -= q;
        o->alive = !gone;
        break;
      }
      case EventType::Remove: {
        OverlayEntry* o = lookup(e.sym, e.id);
        if (!o || !o->alive) break;
        view_reduce(st, o->side, o->price, o->qty, true);
        o->alive = false;
        break;
      }
      case EventType::Modify: {
        OverlayEntry* o = lookup(e.sym, e.id);
        if (!o || !o->alive) break;
        view_reduce(st, o->side, o->price, o->qty, true);
        if (e.qty == 0) {
          o->alive = false;
          break;
        }
        view_add(st, o->side, e.price, e.qty);
        o->price = e.price;
        o->qty = e.qty;
        break;
      }
      case EventType::Replace: {
        OverlayEntry* o = lookup(e.sym, e.id);
        Side side = e.side;
        if (o && o->alive) {
          view_reduce(st, o->side, o->price, o->qty, true);
          o->alive = false;
          if (!e.side_present) side = o->side;
        } else if (!e.side_present) {
          break;  // L3 cannot place it either
        }
        view_add(st, side, e.price, e.qty);
        overlay_.push_back({e.sym, e.new_id, side, e.price, e.qty, true, false, 0, 0});
        break;
      }
      case EventType::ClearSymbol:  // everything after this in the packet sees an empty book
        st.bids = {};
        st.asks = {};
        st.cleared = true;
        for (auto& o : overlay_)
          if (o.sym == e.sym) o.alive = false;
        break;
      case EventType::SetTick:
        break;
    }
  }

  void refill_if_needed(SymbolId s, SymState& st) {
    const auto* sb = book_.book(s);
    if (!st.bids.knows_top(kPublishDepth)) refill<Side::Buy>(s, st.bids, sb ? &sb->bids : nullptr, st.cleared);
    if (!st.asks.knows_top(kPublishDepth)) refill<Side::Sell>(s, st.asks, sb ? &sb->asks : nullptr, st.cleared);
  }

  // Extends a cache with the levels beyond its last entry, as of now:
  // pre-packet L3 levels adjusted by this packet's overlay. With a ClearSymbol
  // earlier in the packet the pre-packet L3 levels no longer exist.
  template <Side S, class View, class Levels>
  void refill(SymbolId sym, View& v, const Levels* lv, bool cleared) {
    ++stats_.refills;
    const bool bounded = v.n > 0;
    const Price bound = bounded ? v.e[v.n - 1].price : 0;
    const auto beyond = [&](Price p) { return !bounded || better<S>(bound, p); };
    deltas_.clear();
    const auto add_delta = [&](Price p, std::int64_t q, std::int32_t c) {
      for (auto& d : deltas_)
        if (d.price == p) {
          d.qty += q;
          d.count += c;
          return;
        }
      deltas_.push_back({p, q, c});
    };
    for (const auto& o : overlay_) {
      if (o.sym != sym || o.side != S) continue;
      if (o.from_l3 && !cleared && beyond(o.orig_price)) add_delta(o.orig_price, -std::int64_t(o.orig_qty), -1);
      if (o.alive && beyond(o.price)) add_delta(o.price, std::int64_t(o.qty), 1);
    }
    std::sort(deltas_.begin(), deltas_.end(), [](const Delta& a, const Delta& b) { return better<S>(a.price, b.price); });
    bool more = false;
    const auto emit = [&](Price p, std::int64_t q, std::int64_t c) {
      if (c <= 0) return true;  // level emptied by this packet
      if (v.n == static_cast<int>(sizeof(v.e) / sizeof(v.e[0]))) {
        more = true;
        return false;
      }
      v.e[v.n++] = {p, static_cast<std::uint64_t>(q), static_cast<std::uint32_t>(c)};
      return true;
    };
    std::size_t di = 0;
    if (lv && !cleared) {
      lv->for_each([&](const Level& l) {
        if (!beyond(l.price)) return true;
        while (di < deltas_.size() && better<S>(deltas_[di].price, l.price)) {
          if (!emit(deltas_[di].price, deltas_[di].qty, deltas_[di].count)) return false;
          ++di;
        }
        std::int64_t q = static_cast<std::int64_t>(l.qty), c = l.count;
        if (di < deltas_.size() && deltas_[di].price == l.price) {
          q += deltas_[di].qty;
          c += deltas_[di].count;
          ++di;
        }
        return emit(l.price, q, c);
      });
    }
    for (; !more && di < deltas_.size(); ++di)
      if (!emit(deltas_[di].price, deltas_[di].qty, deltas_[di].count)) break;
    v.truncated = more;
  }

  template <class OnPublish>
  void publish_if_changed(SymbolId s, SymState& st, OnPublish& on_publish) {
    TopSnapshot t;
    st.bids.copy_top(t.bid, t.nb);
    st.asks.copy_top(t.ask, t.na);
    if (t == st.last) return;
    st.last = t;
    on_publish(s, t);
  }

  template <class View, class Levels>
  static bool side_ok(const View& v, const Levels& lv) {
    int i = 0;
    bool ok = true;
    std::size_t total = 0;
    lv.for_each([&](const Level& l) {
      ++total;
      if (i < v.n) ok &= v.e[i].price == l.price && v.e[i].qty == l.qty && v.e[i].count == l.count;
      ++i;
      return true;
    });
    if (static_cast<std::size_t>(v.n) > total) return false;
    if (!v.truncated && total != static_cast<std::size_t>(v.n)) return false;
    return ok;
  }

  Book& book_;
  std::vector<SymState> states_;
  std::vector<OverlayEntry> overlay_;
  std::vector<SymbolId> touched_;
  std::vector<std::uint32_t> waiting_;
  std::vector<Delta> deltas_;
  Stats stats_;
};

}  // namespace obl::book
