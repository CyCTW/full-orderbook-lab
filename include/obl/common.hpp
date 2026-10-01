#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace obl {

static_assert(std::endian::native == std::endian::little,
              "wire decoding assumes a little-endian host (XDP and OMD-C are little-endian)");

using Price = std::int64_t;     // raw exchange price units (scale given per symbol)
using Qty = std::uint32_t;      // per-order quantity
using OrderId = std::uint64_t;
using SymbolId = std::uint32_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// True if price `a` is strictly better than `b` for side `S`.
template <Side S>
constexpr bool better(Price a, Price b) {
  if constexpr (S == Side::Buy) return a > b;
  else return a < b;
}

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

template <class T>
inline T load_le(const std::uint8_t* p) {
  static_assert(std::is_trivially_copyable_v<T>);
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

template <class T>
inline void store_le(std::uint8_t* p, T v) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(p, &v, sizeof(T));
}

inline std::uint16_t load_be16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         std::uint32_t(p[3]);
}

inline std::uint64_t mix64(std::uint64_t x) {
  // splitmix64 finalizer
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

}  // namespace obl
