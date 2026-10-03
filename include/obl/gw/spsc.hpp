#pragma once

// Single-producer / single-consumer queues between the gateway thread and a helper thread.
//
// The producer and consumer indices live on separate cache lines, and each side keeps a cached
// copy of the other side's index, so in the common case a push or pop touches no cache line the
// other core is writing (the shared index is reloaded only when the cached one says "full" or
// "empty").
//
//   SpscQueue<T>   fixed-size elements (control commands)
//   SpscRing       variable-length byte records (audit log); a record never wraps: if it does not
//                  fit before the end of the buffer, a padding record fills the rest

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace obl::gw {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity_pow2) : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {}

  bool push(T v) {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_cache_ > mask_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h - tail_cache_ > mask_) return false;
    }
    buf_[h & mask_] = std::move(v);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  std::optional<T> pop() {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t == head_cache_) return std::nullopt;
    }
    T v = std::move(buf_[t & mask_]);
    tail_.store(t + 1, std::memory_order_release);
    return v;
  }

 private:
  const std::size_t mask_;
  std::vector<T> buf_;
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // producer
  std::size_t tail_cache_ = 0;                            // producer's view of tail
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // consumer
  std::size_t head_cache_ = 0;                            // consumer's view of head
};

class SpscRing {
 public:
  // Record layout: [u32 size of payload][u32 tag][payload, padded to 8 bytes]. tag 0 = padding.
  static constexpr std::uint32_t kPad = 0;

  explicit SpscRing(std::size_t bytes_pow2) : mask_(bytes_pow2 - 1), buf_(new std::uint8_t[bytes_pow2]) {
    std::memset(buf_.get(), 0, bytes_pow2);  // prefault
  }

  std::size_t capacity() const { return mask_ + 1; }

  // Reserve room for a record; nullptr if the ring is full. Fill the payload, then commit().
  std::uint8_t* reserve(std::uint32_t tag, std::uint32_t size) {
    const std::size_t need = 8 + ((size + 7) & ~std::size_t{7});
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t off = h & mask_;
    const std::size_t to_end = capacity() - off;
    const std::size_t total = need <= to_end ? need : to_end + need;  // padding + record
    if (total > capacity()) return nullptr;
    if (h + total - tail_cache_ > capacity()) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h + total - tail_cache_ > capacity()) return nullptr;
    }
    std::size_t at = h;
    if (need > to_end) {  // pad to the end, record starts at offset 0
      write_header(off, kPad, static_cast<std::uint32_t>(to_end - 8));
      at += to_end;
    }
    write_header(at & mask_, tag, size);
    pending_ = at + need;
    return buf_.get() + (at & mask_) + 8;
  }
  void commit() { head_.store(pending_, std::memory_order_release); }

  // Consumer: calls f(tag, payload, size) for every committed record; returns how many.
  template <class F>
  std::size_t consume(F&& f) {
    std::size_t n = 0;
    std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t h = head_.load(std::memory_order_acquire);
    while (t != h) {
      const std::size_t off = t & mask_;
      std::uint32_t size, tag;
      std::memcpy(&size, buf_.get() + off, 4);
      std::memcpy(&tag, buf_.get() + off + 4, 4);
      if (tag != kPad) {
        f(tag, buf_.get() + off + 8, size);
        ++n;
      }
      t += 8 + ((size + 7) & ~std::size_t{7});
    }
    tail_.store(t, std::memory_order_release);
    return n;
  }

 private:
  void write_header(std::size_t off, std::uint32_t tag, std::uint32_t size) {
    std::memcpy(buf_.get() + off, &size, 4);
    std::memcpy(buf_.get() + off + 4, &tag, 4);
  }

  const std::size_t mask_;
  std::unique_ptr<std::uint8_t[]> buf_;
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
  std::size_t pending_ = 0;
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

}  // namespace obl::gw
