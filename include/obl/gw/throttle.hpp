#pragma once

// Outbound message throttle for one exchange session.
//
// The exchange allows `limit` business messages per second per session (OCG-C: per Comp ID,
// §6.12; excess messages are rejected and persistent offenders disconnected). Every business
// message takes a slot in a one-second sliding window. When the window is full, requests wait in
// three FIFO queues, drained in priority order:
//
//   1. cancels      reduce risk; never dropped
//   2. amends       one per order at most (the order table allows one request in flight per
//                   order), so a newer amend for a queued one just retargets it
//   3. new orders   dropped after max_new_age_ns: a stale order is rejected back to the strategy
//                   rather than sent late
//
// A request may go out at once only if the window has room AND nothing of equal or higher
// priority is waiting, so a cancel can overtake queued new orders but never another cancel.
// The throttle only schedules: the caller encodes and sends.

#include <cstdint>
#include <vector>

#include "obl/gw/order_state.hpp"
#include "obl/gw/rate_limit.hpp"

namespace obl::gw {

struct ThrottleConfig {
  std::uint32_t msgs_per_sec = 0;  // 0 = unlimited
  std::uint64_t window_ns = 1'000'000'000;
  std::uint32_t queue_capacity = 4096;  // per class
  std::uint64_t max_new_age_ns = 50'000'000;  // queued new orders older than this are dropped
};

struct Queued {
  ReqKind kind = ReqKind::None;
  OrderSlot slot = kNoOrder;
  std::uint32_t req_id = 0;
  std::uint64_t since = 0;
};

class Throttle {
 public:
  explicit Throttle(const ThrottleConfig& cfg = {}) { configure(cfg); }

  void configure(const ThrottleConfig& cfg) {
    cfg_ = cfg;
    window_.configure(cfg.msgs_per_sec, cfg.window_ns);
    for (auto& q : q_) q.init(cfg.queue_capacity);
  }

  // May a request of this kind go out right now? If yes, the slot is taken.
  bool admit(ReqKind k, std::uint64_t now) {
    bool blocked = false;
    for (int c = 0; c <= cls(k); ++c) blocked |= !q_[c].empty();
    if (blocked || !window_.try_take(now)) {
      ++throttled_;
      return false;
    }
    return true;
  }

  // Queue a request that was not admitted. Returns false if that queue is full.
  bool enqueue(ReqKind k, OrderSlot slot, std::uint32_t req_id, std::uint64_t now) {
    return q_[cls(k)].push({k, slot, req_id, now});
  }

  // Send queued requests while the window has room. send(const Queued&) returns false to stop
  // (e.g. session down); that request stays queued.
  template <class Send>
  std::uint32_t drain(std::uint64_t now, Send&& send) {
    std::uint32_t n = 0;
    for (int c = 0; c < 3; ++c) {
      while (!q_[c].empty() && window_.allows(now)) {
        if (!send(q_[c].front())) return n;
        window_.try_take(now);
        q_[c].pop();
        ++n;
      }
      if (!q_[c].empty()) return n;  // lower classes wait behind this one
    }
    return n;
  }

  // Drop queued new orders older than max_new_age_ns; drop(const Queued&) is told about each.
  template <class Drop>
  void expire(std::uint64_t now, Drop&& drop) {
    auto& q = q_[2];
    while (!q.empty() && now - q.front().since > cfg_.max_new_age_ns) {
      drop(q.front());
      q.pop();
    }
  }

  // Remove every queued request of a class (session down: everything; kill switch: new orders
  // and amends). drop(const Queued&) is told about each.
  template <class Drop>
  void clear(ReqKind k, Drop&& drop) {
    auto& q = q_[cls(k)];
    while (!q.empty()) {
      drop(q.front());
      q.pop();
    }
  }

  // Remove one queued request (strategy cancels an order that is still queued as new).
  bool remove(ReqKind k, std::uint32_t req_id) { return q_[cls(k)].erase(req_id); }

  // When should drain() be called next? 0 if nothing is queued.
  std::uint64_t next_wakeup(std::uint64_t now) const {
    for (const auto& q : q_)
      if (!q.empty()) return window_.next_allowed(now);
    return 0;
  }

  std::uint32_t queued(ReqKind k) const { return q_[cls(k)].size(); }
  std::uint32_t queued_total() const { return q_[0].size() + q_[1].size() + q_[2].size(); }
  std::uint64_t throttled_count() const { return throttled_; }
  std::uint32_t window_used(std::uint64_t now) const { return window_.used(now); }
  std::uint32_t limit() const { return window_.limit(); }

 private:
  static int cls(ReqKind k) {
    switch (k) {
      case ReqKind::Cancel:
      case ReqKind::MassCancel: return 0;
      case ReqKind::Amend: return 1;
      default: return 2;
    }
  }

  // Fixed-capacity FIFO (no allocation after configure).
  class Ring {
   public:
    void init(std::uint32_t cap) {
      buf_.assign(cap ? cap : 1, Queued{});
      head_ = size_ = 0;
    }
    bool empty() const { return size_ == 0; }
    std::uint32_t size() const { return size_; }
    const Queued& front() const { return buf_[head_]; }
    bool push(const Queued& q) {
      if (size_ == buf_.size()) return false;
      buf_[(head_ + size_) % buf_.size()] = q;
      ++size_;
      return true;
    }
    void pop() {
      head_ = (head_ + 1) % static_cast<std::uint32_t>(buf_.size());
      --size_;
    }
    bool erase(std::uint32_t req_id) {
      const auto cap = static_cast<std::uint32_t>(buf_.size());
      for (std::uint32_t i = 0; i < size_; ++i) {
        if (buf_[(head_ + i) % cap].req_id != req_id) continue;
        for (std::uint32_t j = i; j + 1 < size_; ++j) buf_[(head_ + j) % cap] = buf_[(head_ + j + 1) % cap];
        --size_;
        return true;
      }
      return false;
    }

   private:
    std::vector<Queued> buf_;
    std::uint32_t head_ = 0, size_ = 0;
  };

  ThrottleConfig cfg_;
  SlidingWindow window_;
  Ring q_[3];
  std::uint64_t throttled_ = 0;
};

}  // namespace obl::gw
