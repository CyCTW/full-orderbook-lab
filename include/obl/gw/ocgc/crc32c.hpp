#pragma once

// CRC32C (Castagnoli, polynomial 0x1EDC6F41; reflected 0x82F63B78), the OCG-C trailer checksum.
//
// Variants, slowest to fastest:
//   crc32c_bitwise   one bit per step (reference)
//   crc32c_sarwate   one 256-entry table, one byte per step
//   crc32c_slice8    eight 256-entry tables (8 KiB), eight bytes per step
//   crc32c_hw        SSE4.2 `crc32` instruction, eight bytes per instruction
//
// All take and return the finalized value (init ~0, final xor ~0), so they chain:
// crc32c_x(crc32c_x(0, a), b) == crc32c_x(0, a ++ b).
//
// For incremental updates the raw (non-finalized) register is linear over GF(2):
// crc32c_raw(a ^ b) == crc32c_raw(a) ^ crc32c_raw(b) for equal-length a, b. crc32c_shift
// appends zero bytes to a raw register in O(1) with one carry-less multiply.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif
#if defined(__PCLMUL__)
#include <wmmintrin.h>
#endif

namespace obl::gw {

inline constexpr std::uint32_t kCrc32cPoly = 0x82F63B78u;  // reflected 0x1EDC6F41

inline std::uint32_t crc32c_bitwise(std::uint32_t crc, const void* data, std::size_t n) {
  auto p = static_cast<const std::uint8_t*>(data);
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (kCrc32cPoly & (0u - (crc & 1u)));
  }
  return ~crc;
}

namespace detail {

constexpr std::array<std::array<std::uint32_t, 256>, 8> make_crc32c_tables() {
  std::array<std::array<std::uint32_t, 256>, 8> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (kCrc32cPoly & (0u - (c & 1u)));
    t[0][i] = c;
  }
  for (std::uint32_t i = 0; i < 256; ++i)
    for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
  return t;
}

inline constexpr auto kCrc32cTables = make_crc32c_tables();

}  // namespace detail

inline std::uint32_t crc32c_sarwate(std::uint32_t crc, const void* data, std::size_t n) {
  const auto& t = detail::kCrc32cTables[0];
  auto p = static_cast<const std::uint8_t*>(data);
  crc = ~crc;
  while (n--) crc = (crc >> 8) ^ t[(crc ^ *p++) & 0xFF];
  return ~crc;
}

inline std::uint32_t crc32c_slice8(std::uint32_t crc, const void* data, std::size_t n) {
  const auto& t = detail::kCrc32cTables;
  auto p = static_cast<const std::uint8_t*>(data);
  crc = ~crc;
  while (n >= 8) {
    std::uint64_t w;
    std::memcpy(&w, p, 8);
    w ^= crc;
    crc = t[7][w & 0xFF] ^ t[6][(w >> 8) & 0xFF] ^ t[5][(w >> 16) & 0xFF] ^ t[4][(w >> 24) & 0xFF] ^
          t[3][(w >> 32) & 0xFF] ^ t[2][(w >> 40) & 0xFF] ^ t[1][(w >> 48) & 0xFF] ^ t[0][w >> 56];
    p += 8;
    n -= 8;
  }
  while (n--) crc = (crc >> 8) ^ t[0][(crc ^ *p++) & 0xFF];
  return ~crc;
}

#if defined(__SSE4_2__)

// Raw register update (no init / final xor).
inline std::uint32_t crc32c_hw_raw(std::uint32_t crc, const void* data, std::size_t n) {
  auto p = static_cast<const std::uint8_t*>(data);
  std::uint64_t c = crc;
  while (n >= 8) {
    std::uint64_t w;
    std::memcpy(&w, p, 8);
    c = _mm_crc32_u64(c, w);
    p += 8;
    n -= 8;
  }
  auto c32 = static_cast<std::uint32_t>(c);
  if (n & 4) {
    std::uint32_t w;
    std::memcpy(&w, p, 4);
    c32 = _mm_crc32_u32(c32, w);
    p += 4;
  }
  if (n & 2) {
    std::uint16_t w;
    std::memcpy(&w, p, 2);
    c32 = _mm_crc32_u16(c32, w);
    p += 2;
  }
  if (n & 1) c32 = _mm_crc32_u8(c32, *p);
  return c32;
}

inline std::uint32_t crc32c_hw(std::uint32_t crc, const void* data, std::size_t n) {
  return ~crc32c_hw_raw(~crc, data, n);
}

#endif

// ---------------------------------------------------------------------------------------------
// GF(2) arithmetic mod P in the reflected representation (bit 31 = x^0). Not on the hot path:
// used once per template to build shift constants.

// a * b mod P
inline std::uint32_t crc32c_multmodp(std::uint32_t a, std::uint32_t b) {
  std::uint32_t m = 1u << 31, p = 0;
  for (;;) {
    if (a & m) {
      p ^= b;
      if ((a & (m - 1)) == 0) break;
    }
    m >>= 1;
    b = (b & 1) ? (b >> 1) ^ kCrc32cPoly : b >> 1;
  }
  return p;
}

// x^n mod P
inline std::uint32_t crc32c_xpow(std::uint64_t n) {
  std::uint32_t result = 1u << 31;  // x^0
  std::uint32_t base = 1u << 30;    // x^1
  while (n) {
    if (n & 1) result = crc32c_multmodp(base, result);
    base = crc32c_multmodp(base, base);
    n >>= 1;
  }
  return result;
}

// Raw register after appending `zeros` zero bytes: crc * x^(8*zeros) mod P. Portable, slow.
inline std::uint32_t crc32c_shift_slow(std::uint32_t crc, std::size_t zeros) {
  return crc32c_multmodp(crc32c_xpow(8 * zeros), crc);
}

#if defined(__SSE4_2__) && defined(__PCLMUL__)

// Precomputed "append k zero bytes" for a fixed k. One pclmulqdq + one crc32:
//   clmul(c, K) as a 64-bit reflected value is c * K * x, and crc32_u64(0, v) = v * x^32 mod P,
//   so K = x^(8k - 33) gives c * x^(8k).
struct Crc32cShift {
  std::uint32_t k = 0;      // constant x^(8*zeros - 33) mod P
  std::uint32_t small = 0;  // zeros < 5: shift with crc32_u8 instead
  bool use_small = true;

  Crc32cShift() = default;
  explicit Crc32cShift(std::size_t zeros) {
    if (zeros >= 5) {
      k = crc32c_xpow(8 * zeros - 33);
      use_small = false;
    } else {
      small = static_cast<std::uint32_t>(zeros);
    }
  }

  std::uint32_t operator()(std::uint32_t crc) const {
    if (use_small) {
      for (std::uint32_t i = 0; i < small; ++i) crc = _mm_crc32_u8(crc, 0);
      return crc;
    }
    const __m128i prod = _mm_clmulepi64_si128(_mm_cvtsi32_si128(static_cast<int>(crc)),
                                              _mm_cvtsi32_si128(static_cast<int>(k)), 0x00);
    return static_cast<std::uint32_t>(
        _mm_crc32_u64(0, static_cast<std::uint64_t>(_mm_cvtsi128_si64(prod))));
  }
};

#endif

}  // namespace obl::gw
