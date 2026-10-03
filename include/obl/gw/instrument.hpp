#pragma once

// Instruments the gateway trades: exchange symbol <-> dense index, board lot, tick rules and the
// reference price the risk checks compare against.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace obl::gw {

using SymbolIdx = std::uint32_t;
inline constexpr SymbolIdx kNoSymbol = 0xFFFFFFFFu;

// Prices and quantities are fixed point with 8 decimals (OCG-C "Decimal").
inline constexpr std::int64_t kScale = 100'000'000;

// Notional in 1e-8 currency units: price * qty / 1e8, without overflow for any realistic trade.
__extension__ using int128 = __int128;
inline std::int64_t notional(std::int64_t price, std::int64_t qty) {
  return static_cast<std::int64_t>(static_cast<int128>(price) * qty / kScale);
}

// HKEX securities spread table (Part A, stocks): tick size by price band. Returns 0 outside the
// table's range (price <= 0 or > 9,995).
inline std::int64_t hkex_tick(std::int64_t price) {
  struct Band {
    std::int64_t upto, tick;
  };
  static constexpr Band kBands[] = {
      {25 * kScale / 100, kScale / 1000},  // 0.01 - 0.25   : 0.001
      {50 * kScale / 100, kScale / 200},   // 0.25 - 0.50   : 0.005
      {10 * kScale, kScale / 100},         // 0.50 - 10     : 0.01
      {20 * kScale, kScale / 50},          // 10 - 20       : 0.02
      {100 * kScale, kScale / 20},         // 20 - 100      : 0.05
      {200 * kScale, kScale / 10},         // 100 - 200     : 0.1
      {500 * kScale, kScale / 5},          // 200 - 500     : 0.2
      {1000 * kScale, kScale / 2},         // 500 - 1,000   : 0.5
      {2000 * kScale, kScale},             // 1,000 - 2,000 : 1
      {5000 * kScale, 2 * kScale},         // 2,000 - 5,000 : 2
      {9995 * kScale, 5 * kScale},         // 5,000 - 9,995 : 5
  };
  if (price <= 0) return 0;
  for (const Band& b : kBands)
    if (price <= b.upto) return b.tick;
  return 0;
}

// A price is valid if it is a multiple of the tick of its band. Band edges belong to the lower
// band (e.g. 10.00 is on the 0.01 grid and also on the 0.02 grid).
inline bool hkex_valid_price(std::int64_t price) {
  const std::int64_t tick = hkex_tick(price);
  return tick != 0 && price % tick == 0;
}

struct Instrument {
  std::string security_id;     // exchange symbol, e.g. "700"
  std::int64_t board_lot = 0;  // shares per board lot, fixed point (100 shares = 100 * kScale)
  bool shortable = false;      // on the designated short-sell list
  bool restricted = false;     // on the firm's restricted list: no new orders
  std::int64_t reference_price = 0;  // for price collars; 0 = unknown
};

class InstrumentTable {
 public:
  SymbolIdx add(Instrument ins) {
    const auto idx = static_cast<SymbolIdx>(list_.size());
    by_code_.emplace(ins.security_id, idx);
    list_.push_back(std::move(ins));
    return idx;
  }
  SymbolIdx find(std::string_view code) const {
    auto it = by_code_.find(std::string(code));
    return it == by_code_.end() ? kNoSymbol : it->second;
  }
  std::size_t size() const { return list_.size(); }
  Instrument& operator[](SymbolIdx i) { return list_[i]; }
  const Instrument& operator[](SymbolIdx i) const { return list_[i]; }

 private:
  std::vector<Instrument> list_;
  std::unordered_map<std::string, SymbolIdx> by_code_;
};

}  // namespace obl::gw
