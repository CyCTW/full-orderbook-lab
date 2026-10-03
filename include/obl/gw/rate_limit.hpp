#pragma once

// Rate limiters, all integer nanoseconds, O(1) per message.
//
//   Gcra           token bucket as one timestamp (Generic Cell Rate Algorithm): `rate` per second
//                  sustained, bursts of up to `burst`. Used for the strategy order-rate limit.
//   SlidingWindow  at most `limit` events in ANY window of `window_ns`. Used for the exchange
//                  throttle: OCG-C counts business messages per fixed one-second interval whose
//                  alignment the client does not know (§6.12); keeping every sliding window under
//                  the limit keeps every fixed window under it too.

#include <cstdint>
#include <vector>

namespace obl::gw {

class Gcra {
 public:
  Gcra() = default;
  Gcra(std::uint32_t rate_per_sec, std::uint32_t burst) { configure(rate_per_sec, burst); }

  void configure(std::uint32_t rate_per_sec, std::uint32_t burst) {
    interval_ = rate_per_sec ? 1'000'000'000ull / rate_per_sec : 0;
    tolerance_ = interval_ * (burst ? burst - 1 : 0);
  }
  bool unlimited() const { return interval_ == 0; }

  // Would one more event at `now` conform? (no state change)
  bool allows(std::uint64_t now) const { return unlimited() || now + tolerance_ >= tat_; }

  // Take one event; returns false (and changes nothing) if it would exceed the rate.
  bool try_take(std::uint64_t now) {
    if (unlimited()) return true;
    if (now + tolerance_ < tat_) return false;
    tat_ = (tat_ > now ? tat_ : now) + interval_;
    return true;
  }

 private:
  std::uint64_t interval_ = 0;   // ns per event
  std::uint64_t tolerance_ = 0;  // ns of credit = (burst - 1) intervals
  std::uint64_t tat_ = 0;        // theoretical arrival time of the next event
};

class SlidingWindow {
 public:
  SlidingWindow() = default;
  SlidingWindow(std::uint32_t limit, std::uint64_t window_ns) { configure(limit, window_ns); }

  void configure(std::uint32_t limit, std::uint64_t window_ns) {
    window_ = window_ns;
    times_.assign(limit, 0);
    head_ = 0;
    count_ = 0;
  }
  bool unlimited() const { return times_.empty(); }
  std::uint32_t limit() const { return static_cast<std::uint32_t>(times_.size()); }

  bool allows(std::uint64_t now) const {
    return unlimited() || count_ < times_.size() || now - times_[head_] >= window_;
  }
  // Earliest time an event would be allowed (now if allowed).
  std::uint64_t next_allowed(std::uint64_t now) const {
    if (allows(now)) return now;
    return times_[head_] + window_;
  }
  bool try_take(std::uint64_t now) {
    if (!allows(now)) return false;
    take(now);
    return true;
  }
  // Events in the window ending at `now`.
  std::uint32_t used(std::uint64_t now) const {
    std::uint32_t n = 0;
    const std::size_t cap = times_.size();  // ring is ordered oldest -> newest from oldest()
    for (std::size_t i = 0; i < count_; ++i) {
      const std::uint64_t t = times_[(oldest() + i) % cap];
      if (now - t < window_) ++n;
    }
    return n;
  }

 private:
  // head_ is the slot to overwrite next = the oldest entry once the ring is full
  std::size_t oldest() const { return count_ < times_.size() ? 0 : head_; }
  void take(std::uint64_t now) {
    if (unlimited()) return;
    times_[head_] = now;
    head_ = (head_ + 1) % times_.size();
    if (count_ < times_.size()) ++count_;
  }

  std::uint64_t window_ = 0;
  std::vector<std::uint64_t> times_;
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

}  // namespace obl::gw
