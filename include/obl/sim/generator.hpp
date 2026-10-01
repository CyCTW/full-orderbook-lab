#pragma once
// Synthetic XDP Integrated Feed generator.
//
// Keeps a shadow book per symbol so every emitted message is valid (deletes,
// modifies and executions only reference live orders, books never cross).
// The statistical shape is tunable but defaults roughly follow what L3 equity
// feeds look like: adds ~ cancels >> executions, most activity within a few
// ticks of the touch, a thin tail of far-away orders, Zipf-distributed symbol
// activity, non-consecutive order ids.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <map>
#include <vector>

#include "obl/xdp/messages.hpp"

namespace obl::sim {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : s_(seed ? seed : 1) {}
  std::uint64_t next() { return mix64(s_ += 0x9E3779B97F4A7C15ULL); }
  std::uint64_t below(std::uint64_t n) { return static_cast<std::uint64_t>((__uint128_t(next()) * n) >> 64); }
  double real() { return (next() >> 11) * 0x1.0p-53; }
  bool chance(double p) { return real() < p; }
  // number of failures before first success, success probability p
  std::uint32_t geometric(double p) {
    return static_cast<std::uint32_t>(std::log(1.0 - real()) / std::log(1.0 - p));
  }

 private:
  std::uint64_t s_;
};

struct GenConfig {
  std::uint32_t symbols = 200;
  std::uint64_t messages = 5'000'000;
  std::uint32_t target_orders = 1500;  // steady-state live orders per symbol (scaled by activity)
  double zipf_s = 1.0;
  std::uint64_t seed = 42;
  std::uint32_t tick = 100;            // raw price units per tick (price scale 4 => $0.01)
  double p_add = 0.45, p_delete = 0.38, p_modify = 0.07, p_replace = 0.05, p_execute = 0.05;
  double far_order_prob = 0.01;        // orders placed hundreds/thousands of ticks away
  double symbol_clear_prob = 0.0;      // per message
  std::uint32_t max_msgs_per_packet = 24;
};

// Sink receives one encoded message at a time.
template <class Sink>
class FlowGenerator {
 public:
  FlowGenerator(const GenConfig& cfg, Sink& sink) : cfg_(cfg), sink_(sink), rng_(cfg.seed) {
    double total = 0;
    for (std::uint32_t i = 0; i < cfg_.symbols; ++i) {
      total += 1.0 / std::pow(i + 1.0, cfg_.zipf_s);
      cdf_.push_back(total);
    }
    syms_.resize(cfg_.symbols);
    for (std::uint32_t i = 0; i < cfg_.symbols; ++i) {
      SymState& s = syms_[i];
      s.mid = s.fair = 1000 + static_cast<std::int64_t>(rng_.below(30000));  // $10 .. $310
      const double share = (1.0 / std::pow(i + 1.0, cfg_.zipf_s)) / total;
      s.target = std::max<std::uint32_t>(
          50, static_cast<std::uint32_t>(cfg_.target_orders * std::sqrt(share * cfg_.symbols)));
    }
  }

  void run() {
    std::uint8_t buf[64];
    xdp::SequenceNumberReset reset{0, 0, 1, 1};
    sink_.message(buf, xdp::encode(buf, reset));
    for (std::uint32_t i = 0; i < cfg_.symbols; ++i) {
      xdp::SymbolIndexMapping m{};
      m.symbol_index = sym_id(i);
      char name[24];
      const int n = std::snprintf(name, sizeof(name), "S%u", i);
      std::memcpy(m.symbol, name, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(m.symbol)));
      m.market_id = 1;
      m.exchange_code = 'N';
      m.price_scale_code = 4;
      m.security_type = 'A';
      m.lot_size = 100;
      m.prev_close_price = static_cast<std::uint32_t>(syms_[i].mid * cfg_.tick);
      m.round_lot = 'Y';
      m.mpv = static_cast<std::uint16_t>(cfg_.tick);
      m.unit_of_trade = 100;
      sink_.message(buf, xdp::encode(buf, m));
    }
    sink_.flush();
    for (std::uint64_t n = 0; n < cfg_.messages; ++n) step();
    sink_.flush();
  }

  std::uint64_t live_orders() const { return orders_.size(); }

  // Symbol index on the wire: sparse like real feeds (not 0..N-1).
  static std::uint32_t sym_id(std::uint32_t i) { return 1 + i * 7; }

 private:
  struct Live {
    std::uint32_t sym;
    std::uint32_t price;   // ticks
    std::uint32_t qty;
    Side side;
    std::uint32_t pos_all;
    std::uint32_t pos_level;
  };
  using LevelMap = std::map<std::uint32_t, std::vector<OrderId>>;
  struct SymState {
    std::int64_t mid = 0;  // ticks
    std::int64_t fair = 0; // long-run anchor the price mean-reverts to
    std::uint32_t target = 0;
    std::uint32_t seq = 0;
    std::vector<OrderId> all;
    LevelMap bids, asks;
  };

  std::uint32_t pick_symbol() {
    const double u = rng_.real() * cdf_.back();
    return static_cast<std::uint32_t>(std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
  }

  LevelMap& levels(SymState& s, Side side) { return side == Side::Buy ? s.bids : s.asks; }
  static std::int64_t best(const SymState& s, Side side) {
    // -1 = empty (cast first: the ternary's common type would otherwise be uint32_t)
    if (side == Side::Buy) return s.bids.empty() ? -1 : static_cast<std::int64_t>(s.bids.rbegin()->first);
    return s.asks.empty() ? -1 : static_cast<std::int64_t>(s.asks.begin()->first);
  }

  std::uint32_t pick_price(SymState& s, Side side) {
    const std::int64_t bb = best(s, Side::Buy), ba = best(s, Side::Sell);
    if (bb > 0 && ba > 0) s.mid = (bb + ba) / 2;
    std::int64_t d = rng_.chance(cfg_.far_order_prob) ? 200 + static_cast<std::int64_t>(rng_.below(3000))
                                                      : rng_.geometric(0.3);
    std::int64_t p;
    if (side == Side::Buy) {
      const std::int64_t ref = ba > 0 ? ba - 1 : (bb > 0 ? bb : s.mid);
      p = std::max<std::int64_t>(1, ref - d);
    } else {
      const std::int64_t ref = bb > 0 ? bb + 1 : (ba > 0 ? ba : s.mid);
      p = std::max<std::int64_t>(1, ref + d);
    }
    return static_cast<std::uint32_t>(p);
  }

  // A buy needs room below the best ask (and a sell can always go higher).
  Side feasible(const SymState& s, Side side) const {
    if (side == Side::Buy) {
      const std::int64_t ba = best(s, Side::Sell);
      if (ba >= 0 && ba <= 1) return Side::Sell;
    }
    return side;
  }

  // Executions hit bids more often when the price is above its anchor and
  // asks more often when below, so the random walk stays bounded.
  Side execution_side(const SymState& s) {
    const double dev = static_cast<double>(s.mid - s.fair) / static_cast<double>(s.fair);
    const double p_hit_bid = std::clamp(0.5 + 5.0 * dev, 0.1, 0.9);
    return rng_.chance(p_hit_bid) ? Side::Buy : Side::Sell;
  }

  std::uint32_t pick_qty() {
    if (rng_.chance(0.1)) return 1 + static_cast<std::uint32_t>(rng_.below(99));  // odd lot
    return 100 * (1 + std::min<std::uint32_t>(rng_.geometric(0.35), 50));
  }

  void index_add(SymState& s, OrderId id, Live o) {
    auto& lvl = levels(s, o.side)[o.price];
    o.pos_level = static_cast<std::uint32_t>(lvl.size());
    lvl.push_back(id);
    o.pos_all = static_cast<std::uint32_t>(s.all.size());
    s.all.push_back(id);
    orders_[id] = o;
  }

  void index_remove(SymState& s, OrderId id) {
    const Live o = orders_.at(id);
    auto& lm = levels(s, o.side);
    auto it = lm.find(o.price);
    auto& lvl = it->second;
    const OrderId moved = lvl.back();
    lvl[o.pos_level] = moved;
    orders_[moved].pos_level = o.pos_level;
    lvl.pop_back();
    if (lvl.empty()) lm.erase(it);
    const OrderId moved_all = s.all.back();
    s.all[o.pos_all] = moved_all;
    orders_[moved_all].pos_all = o.pos_all;
    s.all.pop_back();
    orders_.erase(id);
  }

  OrderId new_id() { return next_id_ += 1 + rng_.below(16); }

  std::uint32_t ts() { return static_cast<std::uint32_t>(clock_ns_ += 200 + rng_.below(1600)); }

  void step() {
    const std::uint32_t si = pick_symbol();
    SymState& s = syms_[si];
    const std::uint32_t sym = sym_id(si);
    std::uint8_t buf[64];

    if (cfg_.symbol_clear_prob > 0 && rng_.chance(cfg_.symbol_clear_prob)) {
      while (!s.all.empty()) index_remove(s, s.all.back());
      xdp::SymbolClear m{static_cast<std::uint32_t>(clock_ns_ / 1'000'000'000), ts(), sym, s.seq + 1};
      sink_.message(buf, xdp::encode(buf, m));
      return;
    }

    const std::size_t live = s.all.size();
    double r = rng_.real() * (cfg_.p_add + cfg_.p_delete + cfg_.p_modify + cfg_.p_replace + cfg_.p_execute);
    bool do_add = r < cfg_.p_add;
    if (live < s.target / 2) do_add = do_add || rng_.chance(0.6);
    if (live > s.target * 3 / 2) do_add = false;
    if (live == 0) do_add = true;

    if (do_add) {
      const Side side = feasible(s, rng_.chance(0.5) ? Side::Buy : Side::Sell);
      Live o{sym, pick_price(s, side), pick_qty(), side, 0, 0};
      const OrderId id = new_id();
      index_add(s, id, o);
      xdp::AddOrder m{ts(), sym, ++s.seq, id, o.price * cfg_.tick, o.qty, side};
      sink_.message(buf, xdp::encode(buf, m));
      return;
    }
    r -= cfg_.p_add;
    if (r < 0) r = rng_.real() * cfg_.p_delete;  // forced away from add: treat as delete

    if (r < cfg_.p_delete) {
      const OrderId id = s.all[rng_.below(live)];
      index_remove(s, id);
      xdp::DeleteOrder m{ts(), sym, ++s.seq, id};
      sink_.message(buf, xdp::encode(buf, m));
      return;
    }
    r -= cfg_.p_delete;

    if (r < cfg_.p_modify) {
      const OrderId id = s.all[rng_.below(live)];
      Live o = orders_.at(id);
      const double k = rng_.real();
      std::uint8_t lost = 1;
      if (k < 0.6 && o.qty > 1) {
        o.qty = 1 + static_cast<std::uint32_t>(rng_.below(o.qty - 1));  // reduce, keeps priority
        lost = 0;
        orders_[id].qty = o.qty;
      } else {
        if (k < 0.85) o.qty += pick_qty();  // increase, loses priority
        else if (feasible(s, o.side) == o.side) o.price = pick_price(s, o.side);
        index_remove(s, id);
        index_add(s, id, o);  // re-queued at the back
      }
      xdp::ModifyOrder m{ts(), sym, ++s.seq, id, o.price * cfg_.tick, o.qty, lost, o.side};
      sink_.message(buf, xdp::encode(buf, m));
      return;
    }
    r -= cfg_.p_modify;

    if (r < cfg_.p_replace) {
      const OrderId id = s.all[rng_.below(live)];
      Live o = orders_.at(id);
      index_remove(s, id);
      if (feasible(s, o.side) == o.side) o.price = pick_price(s, o.side);
      o.qty = pick_qty();
      const OrderId nid = new_id();
      index_add(s, nid, o);
      xdp::ReplaceOrder m{ts(), sym, ++s.seq, id, nid, o.price * cfg_.tick, o.qty, o.side};
      sink_.message(buf, xdp::encode(buf, m));
      return;
    }

    // execution against the best level of a random side
    Side side = execution_side(s);
    if (levels(s, side).empty()) side = opposite(side);
    auto& lm = levels(s, side);
    const auto& lvl = side == Side::Buy ? lm.rbegin()->second : lm.begin()->second;
    const OrderId id = lvl[rng_.below(lvl.size())];
    Live& o = orders_.at(id);
    std::uint32_t q = o.qty;
    if (o.qty > 1 && rng_.chance(0.3)) q = 1 + static_cast<std::uint32_t>(rng_.below(o.qty - 1));
    xdp::OrderExecution m{ts(), sym, ++s.seq, id, ++trade_id_, o.price * cfg_.tick, q, 1};
    if (q == o.qty) index_remove(s, id);
    else o.qty -= q;
    sink_.message(buf, xdp::encode(buf, m));
  }

  GenConfig cfg_;
  Sink& sink_;
  Rng rng_;
  std::vector<double> cdf_;
  std::vector<SymState> syms_;
  std::unordered_map<OrderId, Live> orders_;
  OrderId next_id_ = 1'000'000;
  std::uint32_t trade_id_ = 0;
  std::uint64_t clock_ns_ = 34'200ull * 1'000'000'000ull;  // 09:30
};

// Sink that batches messages into XDP packets and hands each packet to `out`.
template <class Out>
class PacketBuilder {
 public:
  PacketBuilder(Out& out, std::uint32_t max_msgs, std::uint64_t seed)
      : out_(out), max_msgs_(max_msgs), rng_(seed ^ 0xABCDEF) {
    new_burst();
  }

  void message(const std::uint8_t* msg, std::size_t len) {
    if (len_ + len > kMaxPayload || count_ >= burst_) flush();
    std::memcpy(buf_ + len_, msg, len);
    len_ += len;
    ++count_;
  }

  void flush() {
    if (count_ == 0) return;
    ts_ns_ += 500 + rng_.below(5000);
    xdp::PacketHeader h{static_cast<std::uint16_t>(len_), 11, static_cast<std::uint8_t>(count_), seq_,
                        static_cast<std::uint32_t>(ts_ns_ / 1'000'000'000), static_cast<std::uint32_t>(ts_ns_ % 1'000'000'000)};
    xdp::encode_packet_header(buf_, h);
    out_.packet(ts_ns_, buf_, len_);
    seq_ += count_;
    ++packets_;
    len_ = xdp::kPacketHeaderSize;
    count_ = 0;
    new_burst();
  }

  std::uint64_t packets() const { return packets_; }

 private:
  static constexpr std::size_t kMaxPayload = 1400;

  void new_burst() {
    burst_ = 1 + std::min<std::uint32_t>(rng_.geometric(0.25), max_msgs_ - 1);
  }

  Out& out_;
  std::uint32_t max_msgs_;
  Rng rng_;
  std::uint8_t buf_[kMaxPayload];
  std::size_t len_ = xdp::kPacketHeaderSize;
  std::uint32_t count_ = 0;
  std::uint32_t burst_ = 1;
  std::uint32_t seq_ = 1;
  std::uint64_t packets_ = 0;
  std::uint64_t ts_ns_ = 1'548'000'000ull * 1'000'000'000ull;
};

}  // namespace obl::sim
