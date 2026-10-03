#pragma once

// Counters the gateway thread increments and a monitoring thread reads, and the control channel
// through which another thread changes limits or engages the kill switch without stopping.
//
// Counter: single writer. inc() is a relaxed load + store (a plain add on x86, no locked
// instruction); readers on other threads see a value at most slightly stale, never torn.
//
// ControlChannel: closures posted by any one control thread, run on the gateway thread inside
// poll(). Risk limits, reference prices, throttle settings and the kill switch reset all go
// through it, so the hot path never takes a lock. (Engaging the kill switch is also possible
// directly from any thread: it is a single atomic.)

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "obl/gw/risk.hpp"
#include "obl/gw/spsc.hpp"

namespace obl::gw {

class Counter {
 public:
  void inc(std::uint64_t n = 1) { v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
  std::uint64_t get() const { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> v_{0};
};

struct GatewayMetrics {
  Counter orders_sent, cancels_sent, amends_sent, mass_cancels_sent;
  Counter acks, fills, cancels, rejects_exchange, business_rejects;
  Counter queued, dropped_session_down, dropped_expired, dropped_kill, dropped_not_sent;
  Counter session_ups, session_downs;
  Counter msgs_in, msgs_out;
  std::array<Counter, 32> risk_rejects;  // by RiskReject

  std::string summary() const {
    char b[512];
    std::snprintf(b, sizeof b,
                  "sent new=%llu cancel=%llu amend=%llu mass=%llu | acks=%llu fills=%llu cancelled=%llu "
                  "rejects=%llu bmr=%llu | queued=%llu dropped(down=%llu expired=%llu kill=%llu notsent=%llu) | "
                  "sessions up=%llu down=%llu | msgs in=%llu out=%llu",
                  u(orders_sent), u(cancels_sent), u(amends_sent), u(mass_cancels_sent), u(acks), u(fills),
                  u(cancels), u(rejects_exchange), u(business_rejects), u(queued), u(dropped_session_down),
                  u(dropped_expired), u(dropped_kill), u(dropped_not_sent), u(session_ups), u(session_downs),
                  u(msgs_in), u(msgs_out));
    std::string s = b;
    for (std::size_t i = 1; i < risk_rejects.size(); ++i)
      if (risk_rejects[i].get())
        s += std::string(" | risk ") + to_string(static_cast<RiskReject>(i)) + "=" + std::to_string(risk_rejects[i].get());
    return s;
  }

 private:
  static unsigned long long u(const Counter& c) { return c.get(); }
};

class ControlChannel {
 public:
  explicit ControlChannel(std::size_t capacity_pow2 = 1024) : q_(capacity_pow2) {}
  // Control thread. False if the queue is full.
  bool post(std::function<void()> f) { return q_.push(std::move(f)); }
  // Gateway thread.
  std::size_t run_pending() {
    std::size_t n = 0;
    while (auto f = q_.pop()) {
      (*f)();
      ++n;
    }
    return n;
  }

 private:
  SpscQueue<std::function<void()>> q_;
};

}  // namespace obl::gw
