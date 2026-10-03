#pragma once

// Pre-encoded New Order (11) for one (security, side): the static fields are written once, and
// sending an order only patches the per-order fields and recomputes the checksum.
//
// Layout of a limit Day order with the mandatory fields (184 bytes):
//
//   off  size  field                           per order?
//     0    54  header (+ presence map)         Sequence Number at 4..8
//    54    21  Client Order ID                 yes (8 digits used)
//    75    12  Submitting Broker ID
//    87    21  Security ID
//   108     1  Security ID Source = 8
//   109     5  Security Exchange = "XHKG"
//   114    25  Transaction Time                yes (time of day; the date is fixed per day)
//   139     1  Side
//   140     1  Order Type = 2 (Limit)
//   141     8  Price                           yes
//   149     8  Order Quantity                  yes
//   157     2  Disclosure Instructions = 1
//   159    21  Submitting BCAN Field
//   180     4  Checksum                        yes
//
// Incremental checksum: the template keeps zeros in every per-order span, so the message is
// template XOR spans and (CRC is affine) crc(msg) = crc(template) ^ sum over spans of
// shift(raw_crc(span), bytes after span). Each span is an independent dependency chain of a few
// crc32 instructions plus one pclmulqdq, instead of one 23-step chain over the whole message.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "obl/gw/ocgc/crc32c.hpp"
#include "obl/gw/ocgc/protocol.hpp"

namespace obl::gw::ocgc {

struct NewOrderStatic {
  std::string_view comp_id;
  std::string_view submitting_broker_id;
  std::string_view security_id;  // e.g. "700"
  std::string_view bcan;         // "CE1234.5678"
  WireSide side = WireSide::Buy;
  std::uint32_t trade_date = 0;  // YYYYMMDD, UTC date stamped in Transaction Time
  Tif tif = Tif::Day;            // Day is the default and is left out of the message
};

struct NewOrderVar {
  std::uint32_t seq;
  std::uint32_t cl_ord_id;  // 10,000,000 .. 99,999,999 (always 8 digits, see below)
  std::uint64_t us_of_day;  // Transaction Time, microseconds since UTC midnight
  std::int64_t price;       // Decimal, 8 implied decimal places
  std::int64_t qty;         // Decimal, 8 implied decimal places
};

class NewOrderTemplate {
 public:
  static constexpr std::size_t kMaxSize = 192;
  // Client Order IDs are numeric, 1..99,999,999, no leading zeroes (§6.6.3.1). Allocating them
  // from 10,000,000 makes every ID exactly 8 digits: one fixed 8-byte span, no length branch.
  static constexpr std::uint32_t kMinClOrdId = 10'000'000;

  explicit NewOrderTemplate(const NewOrderStatic& s) {
    std::memset(buf_, 0, sizeof buf_);
    Writer w(buf_, MsgType::NewOrder, 0, s.comp_id);
    namespace b = new_order;
    namespace fs = field_size;
    clordid_off_ = w.body_offset();
    w.alnum_slot(b::ClOrdId, fs::kClOrdId);
    w.alnum(b::SubmittingBrokerId, s.submitting_broker_id, fs::kBrokerId);
    w.alnum(b::SecurityId, s.security_id, fs::kSecurityId);
    w.u8(b::SecurityIdSource, kSecurityIdSourceExchangeSymbol);
    w.alnum(b::SecurityExchange, "XHKG", fs::kSecurityExchange);
    const std::size_t time_off = w.body_offset();
    char* t = w.alnum_slot(b::TransactTime, fs::kTransactTime);
    format_date(t, s.trade_date);  // [8, 24) stays zero: the per-order span
    time_off_ = time_off + 8;
    w.u8(b::Side, static_cast<std::uint8_t>(s.side));
    w.u8(b::OrdType, static_cast<std::uint8_t>(OrdType::Limit));
    price_off_ = w.body_offset();
    w.decimal(b::Price, 0);
    w.decimal(b::OrderQty, 0);
    if (s.tif != Tif::Day) w.u8(b::TimeInForce, static_cast<std::uint8_t>(s.tif));
    w.u16(b::DisclosureInstructions, kDisclosureNone);
    w.alnum(b::SubmittingBcan, s.bcan, fs::kBcan);
    size_ = w.finish();
    crc_off_ = size_ - kTrailerSize;
    base_crc_ = load_le<std::uint32_t>(buf_ + crc_off_);
#if defined(__SSE4_2__) && defined(__PCLMUL__)
    shift_seq_ = Crc32cShift(crc_off_ - (hdr::kSeqNum + 4));
    shift_clordid_ = Crc32cShift(crc_off_ - (clordid_off_ + 8));
    shift_time_ = Crc32cShift(crc_off_ - (time_off_ + 16));
    shift_px_qty_ = Crc32cShift(crc_off_ - (price_off_ + 16));
#endif
  }

  std::size_t size() const { return size_; }
  const std::uint8_t* data() const { return buf_; }
  std::uint8_t* data() { return buf_; }

  // Patch the per-order fields; checksum not updated.
  void patch(const NewOrderVar& v) {
    store_le<std::uint32_t>(buf_ + hdr::kSeqNum, v.seq);
    write_clordid(reinterpret_cast<char*>(buf_ + clordid_off_), v.cl_ord_id);
    char* t = reinterpret_cast<char*>(buf_ + time_off_);
    t[0] = '-';
    format_time_of_day(t + 1, v.us_of_day);
    store_le<std::int64_t>(buf_ + price_off_, v.price);
    store_le<std::int64_t>(buf_ + price_off_ + 8, v.qty);
  }

#if defined(__SSE4_2__)
  // Patch + full checksum over the whole message.
  const std::uint8_t* fill(const NewOrderVar& v) {
    patch(v);
    store_le<std::uint32_t>(buf_ + crc_off_, crc32c_hw(0, buf_, crc_off_));
    return buf_;
  }
#endif

#if defined(__SSE4_2__) && defined(__PCLMUL__)
  // Patch + incremental checksum over the per-order spans only.
  const std::uint8_t* fill_incremental(const NewOrderVar& v) {
    patch(v);
    store_le<std::uint32_t>(buf_ + crc_off_, incremental_crc());
    return buf_;
  }

  // Same result as fill_incremental, but every per-order span is built as 64-bit words in
  // registers, stored once and fed to crc32 straight from the register. fill_incremental re-reads
  // spans that were just written with 2-byte stores, which cannot be store-forwarded to an 8-byte
  // load and stalls until the stores commit.
  const std::uint8_t* fill_regs(const NewOrderVar& v) {
    const std::uint64_t id = clordid_word(v.cl_ord_id);
    std::uint64_t t0, t1;
    time_words(v.us_of_day, t0, t1);
    const auto px = static_cast<std::uint64_t>(v.price);
    const auto qty = static_cast<std::uint64_t>(v.qty);
    store_le<std::uint32_t>(buf_ + hdr::kSeqNum, v.seq);
    store_le<std::uint64_t>(buf_ + clordid_off_, id);
    store_le<std::uint64_t>(buf_ + time_off_, t0);
    store_le<std::uint64_t>(buf_ + time_off_ + 8, t1);
    store_le<std::uint64_t>(buf_ + price_off_, px);
    store_le<std::uint64_t>(buf_ + price_off_ + 8, qty);
    const std::uint32_t c_seq = _mm_crc32_u32(0, v.seq);
    const auto c_id = static_cast<std::uint32_t>(_mm_crc32_u64(0, id));
    const auto c_tm = static_cast<std::uint32_t>(_mm_crc32_u64(_mm_crc32_u64(0, t0), t1));
    const auto c_pq = static_cast<std::uint32_t>(_mm_crc32_u64(_mm_crc32_u64(0, px), qty));
    store_le<std::uint32_t>(buf_ + crc_off_, base_crc_ ^ shift_seq_(c_seq) ^ shift_clordid_(c_id) ^
                                                 shift_time_(c_tm) ^ shift_px_qty_(c_pq));
    return buf_;
  }

  std::uint32_t incremental_crc() const {
    const std::uint32_t seq = _mm_crc32_u32(0, load_le<std::uint32_t>(buf_ + hdr::kSeqNum));
    const auto id = static_cast<std::uint32_t>(_mm_crc32_u64(0, load_le<std::uint64_t>(buf_ + clordid_off_)));
    const auto tm = static_cast<std::uint32_t>(
        _mm_crc32_u64(_mm_crc32_u64(0, load_le<std::uint64_t>(buf_ + time_off_)),
                      load_le<std::uint64_t>(buf_ + time_off_ + 8)));
    const auto pq = static_cast<std::uint32_t>(
        _mm_crc32_u64(_mm_crc32_u64(0, load_le<std::uint64_t>(buf_ + price_off_)),
                      load_le<std::uint64_t>(buf_ + price_off_ + 8)));
    return base_crc_ ^ shift_seq_(seq) ^ shift_clordid_(id) ^ shift_time_(tm) ^ shift_px_qty_(pq);
  }
#endif

  std::size_t clordid_offset() const { return clordid_off_; }
  std::size_t price_offset() const { return price_off_; }

 private:
  static std::uint64_t pair(std::uint32_t v) {
    std::uint16_t p;
    std::memcpy(&p, detail::kDigitPairs + 2 * v, 2);
    return p;
  }
  // 8 ASCII digits as one little-endian word.
  static std::uint64_t clordid_word(std::uint32_t id) {
    assert(id >= kMinClOrdId && id <= kMaxClOrdId);
    return pair(id / 1000000) | pair(id / 10000 % 100) << 16 | pair(id / 100 % 100) << 32 |
           pair(id % 100) << 48;
  }
  // "-HH:MM:SS" and ".ssssss" + 'S' digit split across two words: bytes [0,8) and [8,16) of
  // "-HH:MM:SS.ssssss".
  static void time_words(std::uint64_t us_of_day, std::uint64_t& w0, std::uint64_t& w1) {
    const auto us = static_cast<std::uint32_t>(us_of_day % 1'000'000);
    const auto secs = static_cast<std::uint32_t>(us_of_day / 1'000'000);
    const std::uint64_t ss = pair(secs % 60);
    w0 = std::uint64_t('-') | pair(secs / 3600) << 8 | std::uint64_t(':') << 24 | pair(secs / 60 % 60) << 32 |
         std::uint64_t(':') << 48 | (ss & 0xFF) << 56;
    w1 = (ss >> 8) | std::uint64_t('.') << 8 | pair(us / 10000) << 16 | pair(us / 100 % 100) << 32 |
         pair(us % 100) << 48;
  }

  static void write_clordid(char* out, std::uint32_t id) {
    assert(id >= kMinClOrdId && id <= kMaxClOrdId);
    detail::put2(out + 0, id / 1000000);
    detail::put2(out + 2, id / 10000 % 100);
    detail::put2(out + 4, id / 100 % 100);
    detail::put2(out + 6, id % 100);
  }

  alignas(64) std::uint8_t buf_[kMaxSize];
  std::size_t size_ = 0;
  std::size_t crc_off_ = 0;
  std::size_t clordid_off_ = 0;
  std::size_t time_off_ = 0;  // the '-' before HH:MM:SS.ssssss; span is 16 bytes
  std::size_t price_off_ = 0;
  std::uint32_t base_crc_ = 0;
#if defined(__SSE4_2__) && defined(__PCLMUL__)
  Crc32cShift shift_seq_, shift_clordid_, shift_time_, shift_px_qty_;
#endif
};

// Reference encoder: builds the same message from scratch with Writer (presence map, every field,
// generic number formatting). Used by tests to check the template, and by the benchmark as the
// "no template" baseline.
inline std::size_t encode_new_order(std::uint8_t* buf, const NewOrderStatic& s, const NewOrderVar& v) {
  namespace b = new_order;
  namespace fs = field_size;
  Writer w(buf, MsgType::NewOrder, v.seq, s.comp_id);
  char* id = w.alnum_slot(b::ClOrdId, fs::kClOrdId);
  std::to_chars(id, id + fs::kClOrdId - 1, v.cl_ord_id);
  w.alnum(b::SubmittingBrokerId, s.submitting_broker_id, fs::kBrokerId);
  w.alnum(b::SecurityId, s.security_id, fs::kSecurityId);
  w.u8(b::SecurityIdSource, kSecurityIdSourceExchangeSymbol);
  w.alnum(b::SecurityExchange, "XHKG", fs::kSecurityExchange);
  format_transact_time(w.alnum_slot(b::TransactTime, fs::kTransactTime), s.trade_date, v.us_of_day);
  w.u8(b::Side, static_cast<std::uint8_t>(s.side));
  w.u8(b::OrdType, static_cast<std::uint8_t>(OrdType::Limit));
  w.decimal(b::Price, v.price);
  w.decimal(b::OrderQty, v.qty);
  if (s.tif != Tif::Day) w.u8(b::TimeInForce, static_cast<std::uint8_t>(s.tif));
  w.u16(b::DisclosureInstructions, kDisclosureNone);
  w.alnum(b::SubmittingBcan, s.bcan, fs::kBcan);
  return w.finish();
}

}  // namespace obl::gw::ocgc
