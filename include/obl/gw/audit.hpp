#pragma once

// Audit log: every message sent to or received from the exchange, timestamped, on disk.
//
// The gateway thread only copies the message into an SPSC ring (after the bytes have gone to the
// socket); a writer thread drains the ring to a file. If the ring is full (the writer has fallen
// far behind), the default is to spin until there is room: an incomplete audit trail is not an
// option for a regulated gateway, and a full 64 MiB ring means the disk has stalled. Dropping
// instead (counted) is available for tests and research runs.
//
// File: "OBLAUD01", then records [u64 ts_ns][u8 dir][u8 route][u16 length][message bytes].

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "obl/gw/spsc.hpp"

namespace obl::gw {

enum class AuditDir : std::uint8_t { In = 1, Out = 2 };

inline constexpr char kAuditMagic[8] = {'O', 'B', 'L', 'A', 'U', 'D', '0', '1'};

struct AuditConfig {
  std::string path;
  std::size_t ring_bytes = 64u << 20;  // power of two
  bool drop_when_full = false;
  std::chrono::milliseconds flush_every{10};
};

class AuditLog {
 public:
  AuditLog() = default;
  AuditLog(const AuditLog&) = delete;
  AuditLog& operator=(const AuditLog&) = delete;
  ~AuditLog() { close(); }

  bool open(const AuditConfig& cfg) {
    cfg_ = cfg;
    f_ = std::fopen(cfg.path.c_str(), "wb");
    if (!f_) return false;
    std::fwrite(kAuditMagic, 1, 8, f_);
    ring_ = std::make_unique<SpscRing>(cfg.ring_bytes);
    stop_.store(false);
    thread_ = std::thread([this] { writer(); });
    return true;
  }

  // Gateway thread.
  void log(AuditDir dir, std::uint8_t route, std::uint64_t ts, const std::uint8_t* msg, std::size_t len) {
    if (!ring_) return;
    const auto size = static_cast<std::uint32_t>(12 + len);
    std::uint8_t* p = ring_->reserve(1, size);
    while (!p) {
      if (cfg_.drop_when_full) {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
      }
      std::this_thread::yield();
      p = ring_->reserve(1, size);
    }
    std::memcpy(p, &ts, 8);
    p[8] = static_cast<std::uint8_t>(dir);
    p[9] = route;
    const auto l16 = static_cast<std::uint16_t>(len);
    std::memcpy(p + 10, &l16, 2);
    std::memcpy(p + 12, msg, len);
    ring_->commit();
  }

  void close() {
    if (!f_) return;
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    std::fclose(f_);
    f_ = nullptr;
  }

  std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
  std::uint64_t written() const { return written_.load(std::memory_order_relaxed); }

 private:
  void writer() {
    auto last_flush = std::chrono::steady_clock::now();
    for (;;) {
      const bool stopping = stop_.load();
      const std::size_t n = ring_->consume([&](std::uint32_t, const std::uint8_t* p, std::uint32_t size) {
        std::fwrite(p, 1, size, f_);
      });
      written_.fetch_add(n, std::memory_order_relaxed);
      const auto now = std::chrono::steady_clock::now();
      if (now - last_flush >= cfg_.flush_every) {
        std::fflush(f_);
        last_flush = now;
      }
      if (stopping && n == 0) break;
      if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    std::fflush(f_);
  }

  AuditConfig cfg_;
  std::FILE* f_ = nullptr;
  std::unique_ptr<SpscRing> ring_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> dropped_{0}, written_{0};
};

struct AuditRecord {
  std::uint64_t ts;
  AuditDir dir;
  std::uint8_t route;
  std::vector<std::uint8_t> msg;
};

// Reads an audit file back. Returns false if the file is not an audit log.
template <class F>
bool read_audit(const std::string& path, F&& f) {
  std::FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  char magic[8];
  if (std::fread(magic, 1, 8, in) != 8 || std::memcmp(magic, kAuditMagic, 8) != 0) {
    std::fclose(in);
    return false;
  }
  std::uint8_t h[12];
  while (std::fread(h, 1, 12, in) == 12) {
    AuditRecord r;
    std::memcpy(&r.ts, h, 8);
    r.dir = static_cast<AuditDir>(h[8]);
    r.route = h[9];
    std::uint16_t len;
    std::memcpy(&len, h + 10, 2);
    r.msg.resize(len);
    if (std::fread(r.msg.data(), 1, len, in) != len) break;
    f(r);
  }
  std::fclose(in);
  return true;
}

}  // namespace obl::gw
