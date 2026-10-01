#pragma once
// Minimal classic-pcap reader/writer that extracts IPv4/UDP payloads.
// Supports: microsecond and nanosecond pcap (either byte order), link types
// Ethernet (with 802.1Q/QinQ tags), Linux cooked (SLL), raw IPv4.
// `.gz` files are read through zlib when available. pcapng is not supported
// (convert with `editcap -F pcap in.pcapng out.pcap`).

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef OBL_HAVE_ZLIB
#include <zlib.h>
#endif

#include "obl/common.hpp"

namespace obl::pcap {

struct Datagram {
  std::uint64_t ts_ns;
  std::uint32_t dst_ip;    // host order
  std::uint16_t dst_port;
  std::uint32_t offset;    // payload offset into Capture::bytes
  std::uint32_t length;

  // Key for sequence tracking. A and B lines of one channel share a port but
  // use different multicast groups, so arbitration needs the port-only key.
  std::uint64_t stream_key(bool by_group) const {
    return by_group ? (std::uint64_t(dst_ip) << 16) | dst_port : dst_port;
  }
};

struct Capture {
  std::vector<std::uint8_t> bytes;
  std::vector<Datagram> datagrams;
  std::uint64_t frames = 0;
  std::uint64_t non_udp = 0;
  std::uint64_t truncated = 0;

  const std::uint8_t* payload(const Datagram& d) const { return bytes.data() + d.offset; }
};

inline bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

inline std::vector<std::uint8_t> read_file(const std::string& path) {
  std::vector<std::uint8_t> out;
  if (ends_with(path, ".gz")) {
#ifdef OBL_HAVE_ZLIB
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    gzbuffer(f, 1 << 20);
    std::uint8_t buf[1 << 16];
    int n;
    while ((n = gzread(f, buf, sizeof(buf))) > 0) out.insert(out.end(), buf, buf + n);
    const bool err = n < 0;
    gzclose(f);
    if (err) throw std::runtime_error("gzip read error in " + path);
    return out;
#else
    throw std::runtime_error("built without zlib; gunzip " + path + " first");
#endif
  }
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::fseek(f.get(), 0, SEEK_END);
  const long size = std::ftell(f.get());
  std::fseek(f.get(), 0, SEEK_SET);
  out.resize(static_cast<std::size_t>(size));
  if (size > 0 && std::fread(out.data(), 1, out.size(), f.get()) != out.size())
    throw std::runtime_error("short read on " + path);
  return out;
}

namespace detail {

inline std::uint32_t bswap32(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint16_t bswap16(std::uint16_t v) { return __builtin_bswap16(v); }

// Parses an IPv4 packet; on UDP appends a datagram. Returns false if not UDP.
inline bool parse_ipv4(Capture& cap, std::size_t off, std::size_t len, std::uint64_t ts_ns) {
  const std::uint8_t* ip = cap.bytes.data() + off;
  if (len < 20 || (ip[0] >> 4) != 4) return false;
  const std::size_t ihl = (ip[0] & 0x0F) * 4u;
  const std::size_t total = load_be16(ip + 2);
  if (ip[9] != 17 || ihl < 20 || len < ihl + 8) return false;
  const std::uint16_t frag = load_be16(ip + 6);
  if ((frag & 0x3FFF) != 0) return false;  // fragments are not reassembled
  const std::uint8_t* udp = ip + ihl;
  std::size_t udp_len = load_be16(udp + 4);
  if (udp_len < 8) return false;
  std::size_t avail = len - ihl;
  if (total >= ihl && total - ihl < avail) avail = total - ihl;  // strip Ethernet padding
  if (udp_len > avail) {
    ++cap.truncated;
    udp_len = avail;
  }
  cap.datagrams.push_back({ts_ns, load_be32(ip + 16), load_be16(udp + 2),
                           static_cast<std::uint32_t>(off + ihl + 8),
                           static_cast<std::uint32_t>(udp_len - 8)});
  return true;
}

}  // namespace detail

inline Capture load(const std::string& path) {
  Capture cap;
  cap.bytes = read_file(path);
  const auto& b = cap.bytes;
  if (b.size() < 24) throw std::runtime_error("not a pcap file: " + path);
  const std::uint32_t magic = load_le<std::uint32_t>(b.data());
  bool swap = false, nanos = false;
  switch (magic) {
    case 0xa1b2c3d4: break;
    case 0xa1b23c4d: nanos = true; break;
    case 0xd4c3b2a1: swap = true; break;
    case 0x4d3cb2a1: swap = nanos = true; break;
    case 0x0a0d0d0a: throw std::runtime_error("pcapng not supported, convert with editcap -F pcap");
    default: throw std::runtime_error("unknown pcap magic in " + path);
  }
  auto u32 = [&](std::size_t off) {
    std::uint32_t v = load_le<std::uint32_t>(b.data() + off);
    return swap ? detail::bswap32(v) : v;
  };
  const std::uint32_t linktype = u32(20) & 0x0FFFFFFF;
  if (b.size() > 0xFFFFFFFFull) throw std::runtime_error("captures over 4 GiB are not supported");

  std::size_t pos = 24;
  while (pos + 16 <= b.size()) {
    const std::uint64_t sec = u32(pos), frac = u32(pos + 4);
    const std::size_t incl = u32(pos + 8);
    pos += 16;
    if (pos + incl > b.size()) {
      ++cap.truncated;
      break;
    }
    ++cap.frames;
    const std::uint64_t ts_ns = sec * 1'000'000'000ull + (nanos ? frac : frac * 1000);
    std::size_t off = pos, len = incl;
    bool ok = false;
    if (linktype == 1) {  // Ethernet
      if (len >= 14) {
        std::uint16_t et = load_be16(b.data() + off + 12);
        std::size_t hdr = 14;
        while ((et == 0x8100 || et == 0x88A8) && len >= hdr + 4) {
          et = load_be16(b.data() + off + hdr + 2);
          hdr += 4;
        }
        if (et == 0x0800) ok = detail::parse_ipv4(cap, off + hdr, len - hdr, ts_ns);
      }
    } else if (linktype == 113) {  // Linux cooked
      if (len >= 16 && load_be16(b.data() + off + 14) == 0x0800)
        ok = detail::parse_ipv4(cap, off + 16, len - 16, ts_ns);
    } else if (linktype == 101 || linktype == 228) {  // raw IP / raw IPv4
      ok = detail::parse_ipv4(cap, off, len, ts_ns);
    } else {
      throw std::runtime_error("unsupported pcap link type " + std::to_string(linktype));
    }
    if (!ok) ++cap.non_udp;
    pos += incl;
  }
  return cap;
}

// Writes Ethernet/IPv4/UDP frames into a nanosecond pcap.
class Writer {
 public:
  explicit Writer(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
    if (!f_) throw std::runtime_error("cannot create " + path);
    std::uint8_t h[24] = {};
    store_le<std::uint32_t>(h, 0xa1b23c4d);
    store_le<std::uint16_t>(h + 4, 2);
    store_le<std::uint16_t>(h + 6, 4);
    store_le<std::uint32_t>(h + 16, 65535);
    store_le<std::uint32_t>(h + 20, 1);
    std::fwrite(h, 1, sizeof(h), f_);
  }
  ~Writer() {
    if (f_) std::fclose(f_);
  }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  void write_udp(std::uint64_t ts_ns, std::uint32_t src_ip, std::uint32_t dst_ip, std::uint16_t src_port,
                 std::uint16_t dst_port, const std::uint8_t* payload, std::size_t len) {
    const std::size_t frame = 14 + 20 + 8 + len;
    buf_.assign(16 + frame, 0);
    std::uint8_t* r = buf_.data();
    store_le<std::uint32_t>(r, static_cast<std::uint32_t>(ts_ns / 1'000'000'000ull));
    store_le<std::uint32_t>(r + 4, static_cast<std::uint32_t>(ts_ns % 1'000'000'000ull));
    store_le<std::uint32_t>(r + 8, static_cast<std::uint32_t>(frame));
    store_le<std::uint32_t>(r + 12, static_cast<std::uint32_t>(frame));
    std::uint8_t* e = r + 16;
    // multicast destination MAC 01:00:5e + low 23 bits of group
    e[0] = 0x01; e[1] = 0x00; e[2] = 0x5e;
    e[3] = (dst_ip >> 16) & 0x7F; e[4] = (dst_ip >> 8) & 0xFF; e[5] = dst_ip & 0xFF;
    e[6] = 0x02; e[11] = 0x01;
    e[12] = 0x08; e[13] = 0x00;
    std::uint8_t* ip = e + 14;
    ip[0] = 0x45;
    put_be16(ip + 2, static_cast<std::uint16_t>(20 + 8 + len));
    ip[8] = 32;   // TTL
    ip[9] = 17;   // UDP
    put_be32(ip + 12, src_ip);
    put_be32(ip + 16, dst_ip);
    std::uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += load_be16(ip + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    put_be16(ip + 10, static_cast<std::uint16_t>(~sum));
    std::uint8_t* udp = ip + 20;
    put_be16(udp, src_port);
    put_be16(udp + 2, dst_port);
    put_be16(udp + 4, static_cast<std::uint16_t>(8 + len));  // checksum 0 = none
    std::memcpy(udp + 8, payload, len);
    std::fwrite(buf_.data(), 1, buf_.size(), f_);
  }

 private:
  static void put_be16(std::uint8_t* p, std::uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
  static void put_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
  }

  FILE* f_;
  std::vector<std::uint8_t> buf_;
};

}  // namespace obl::pcap
