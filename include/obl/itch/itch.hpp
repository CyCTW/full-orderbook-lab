#pragma once
// Nasdaq TotalView-ITCH 5.0: message decoding and a streaming reader for the
// length-prefixed sample files published at emi.nasdaq.com
// (each message is preceded by a 2-byte big-endian length).
//
// All integers are BIG-endian (unlike XDP / OMD-C). Prices are u32 with four
// implied decimals. Order reference numbers are unique for the whole day.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef OBL_HAVE_ZLIB
#include <zlib.h>
#endif

#include "obl/common.hpp"

namespace obl::itch {

inline std::uint64_t load_be48(const std::uint8_t* p) {
  return (std::uint64_t(load_be16(p)) << 32) | load_be32(p + 2);
}
inline std::uint64_t load_be64(const std::uint8_t* p) {
  return (std::uint64_t(load_be32(p)) << 32) | load_be32(p + 4);
}

// Common header: Type @0 | StockLocate u16 @1 | TrackingNumber u16 @3 | Timestamp u48 @5 (ns since midnight)
inline std::uint16_t locate(const std::uint8_t* m) { return load_be16(m + 1); }
inline std::uint64_t timestamp(const std::uint8_t* m) { return load_be48(m + 5); }

// Expected message lengths (ITCH 5.0 specification). 0 = unknown type.
inline constexpr std::uint16_t expected_size(std::uint8_t type) {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default: return 0;
  }
}

struct StockDirectory {   // 'R'
  std::uint16_t locate;
  char stock[8];          // right-padded with spaces
  std::uint32_t round_lot;

  std::string symbol() const {
    std::size_t n = 8;
    while (n > 0 && stock[n - 1] == ' ') --n;
    return std::string(stock, n);
  }
};
inline StockDirectory decode_stock_directory(const std::uint8_t* m) {
  StockDirectory d{};
  d.locate = locate(m);
  std::memcpy(d.stock, m + 11, 8);
  d.round_lot = load_be32(m + 21);
  return d;
}

struct AddOrder {         // 'A' (36) and 'F' (40, + attribution @36)
  std::uint16_t locate;
  std::uint64_t ref;
  Side side;
  std::uint32_t shares;
  std::uint32_t price;
};
inline AddOrder decode_add(const std::uint8_t* m) {
  return {locate(m), load_be64(m + 11), m[19] == 'S' ? Side::Sell : Side::Buy, load_be32(m + 20),
          load_be32(m + 32)};
}

struct Executed {         // 'E' (31) and 'C' (36, with printable @31 and price @32)
  std::uint16_t locate;
  std::uint64_t ref;
  std::uint32_t shares;
};
inline Executed decode_executed(const std::uint8_t* m) { return {locate(m), load_be64(m + 11), load_be32(m + 19)}; }

struct Cancel {           // 'X' (23): partial cancel
  std::uint16_t locate;
  std::uint64_t ref;
  std::uint32_t shares;
};
inline Cancel decode_cancel(const std::uint8_t* m) { return {locate(m), load_be64(m + 11), load_be32(m + 19)}; }

struct Delete {           // 'D' (19)
  std::uint16_t locate;
  std::uint64_t ref;
};
inline Delete decode_delete(const std::uint8_t* m) { return {locate(m), load_be64(m + 11)}; }

struct Replace {          // 'U' (35): new order inherits side and stock, loses priority
  std::uint16_t locate;
  std::uint64_t orig_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};
inline Replace decode_replace(const std::uint8_t* m) {
  return {locate(m), load_be64(m + 11), load_be64(m + 19), load_be32(m + 27), load_be32(m + 31)};
}

// Streams messages out of a (gzipped) length-prefixed ITCH file.
class FileReader {
 public:
  explicit FileReader(const std::string& path) {
#ifdef OBL_HAVE_ZLIB
    f_ = gzopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open " + path);
    gzbuffer(f_, 1 << 20);
#else
    throw std::runtime_error("built without zlib");
#endif
    buf_.resize(1 << 22);
  }
  ~FileReader() {
#ifdef OBL_HAVE_ZLIB
    if (f_) gzclose(f_);
#endif
  }
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  // Points `msg` at the next message (valid until the next call). False at EOF.
  bool next(const std::uint8_t*& msg, std::uint16_t& len) {
    if (!ensure(2)) return false;
    len = load_be16(buf_.data() + pos_);
    if (!ensure(2 + std::size_t(len))) {
      if (end_ - pos_ > 0) ++truncated_;
      return false;
    }
    msg = buf_.data() + pos_ + 2;
    pos_ += 2 + std::size_t(len);
    bytes_ += 2 + std::size_t(len);
    return true;
  }

  std::uint64_t bytes() const { return bytes_; }
  std::uint64_t truncated() const { return truncated_; }

 private:
  bool ensure(std::size_t n) {
    if (end_ - pos_ >= n) return true;
    std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
    end_ -= pos_;
    pos_ = 0;
#ifdef OBL_HAVE_ZLIB
    while (end_ < n) {
      const int r = gzread(f_, buf_.data() + end_, static_cast<unsigned>(buf_.size() - end_));
      if (r < 0) throw std::runtime_error("gzip read error");
      if (r == 0) return false;
      end_ += static_cast<std::size_t>(r);
    }
#endif
    return end_ >= n;
  }

#ifdef OBL_HAVE_ZLIB
  gzFile f_ = nullptr;
#endif
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0, end_ = 0;
  std::uint64_t bytes_ = 0, truncated_ = 0;
};

}  // namespace obl::itch
