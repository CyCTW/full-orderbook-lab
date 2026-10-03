// Exchange throttle: sliding window, priority queues, expiry; order-table amend retargeting.

#include <cstdint>
#include <vector>

#include "check.hpp"
#include "obl/gw/throttle.hpp"

using namespace obl::gw;

namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::int64_t S = kScale;

void test_unlimited() {
  Throttle t;
  for (int i = 0; i < 1000; ++i) CHECK(t.admit(ReqKind::New, 0));
  CHECK_EQ(t.next_wakeup(0), 0u);
}

void test_priority_and_drain() {
  ThrottleConfig c;
  c.msgs_per_sec = 3;
  c.max_new_age_ns = 2000 * kMs;
  Throttle t(c);
  CHECK(t.admit(ReqKind::New, 0));
  CHECK(t.admit(ReqKind::New, 1 * kMs));
  CHECK(t.admit(ReqKind::Amend, 2 * kMs));
  CHECK(!t.admit(ReqKind::New, 3 * kMs));  // window full
  CHECK(t.enqueue(ReqKind::New, 10, 100, 3 * kMs));
  CHECK(!t.admit(ReqKind::Cancel, 4 * kMs));
  CHECK(t.enqueue(ReqKind::Cancel, 11, 101, 4 * kMs));
  CHECK(!t.admit(ReqKind::Amend, 5 * kMs));
  CHECK(t.enqueue(ReqKind::Amend, 12, 102, 5 * kMs));
  CHECK_EQ(t.queued_total(), 3u);
  CHECK_EQ(t.throttled_count(), 3u);
  CHECK_EQ(t.next_wakeup(500 * kMs), 1000 * kMs);  // first slot frees at t = 1 s

  // nothing to send before the window moves
  std::vector<std::uint32_t> sent;
  auto send = [&](const Queued& q) {
    sent.push_back(q.req_id);
    return true;
  };
  CHECK_EQ(t.drain(999 * kMs, send), 0u);
  // at 1.000 s one slot, 1.001 s another, 1.002 s the third: cancel, amend, new
  CHECK_EQ(t.drain(1000 * kMs, send), 1u);
  CHECK_EQ(t.drain(1001 * kMs, send), 1u);
  CHECK_EQ(t.drain(1002 * kMs, send), 1u);
  CHECK((sent == std::vector<std::uint32_t>{101, 102, 100}));
  CHECK_EQ(t.queued_total(), 0u);

  // a new order may not overtake a queued new order even if the window has room...
  t.enqueue(ReqKind::New, 13, 103, 1500 * kMs);
  CHECK(!t.admit(ReqKind::New, 2100 * kMs));
  // ...but a cancel may
  CHECK(t.admit(ReqKind::Cancel, 2100 * kMs));
  // send() refusing (session down) keeps the request queued
  CHECK_EQ(t.drain(2100 * kMs, [](const Queued&) { return false; }), 0u);
  CHECK_EQ(t.queued(ReqKind::New), 1u);
}

void test_expire_clear_remove() {
  ThrottleConfig c;
  c.msgs_per_sec = 1;
  c.max_new_age_ns = 50 * kMs;
  Throttle t(c);
  CHECK(t.admit(ReqKind::New, 0));
  t.enqueue(ReqKind::New, 1, 201, 10 * kMs);
  t.enqueue(ReqKind::New, 2, 202, 40 * kMs);
  t.enqueue(ReqKind::Cancel, 3, 203, 40 * kMs);
  std::vector<std::uint32_t> dropped;
  auto drop = [&](const Queued& q) { dropped.push_back(q.req_id); };
  t.expire(70 * kMs, drop);  // 201 is 60 ms old
  CHECK((dropped == std::vector<std::uint32_t>{201}));
  CHECK(t.remove(ReqKind::New, 202));
  CHECK(!t.remove(ReqKind::New, 202));
  CHECK_EQ(t.queued(ReqKind::New), 0u);
  dropped.clear();
  t.clear(ReqKind::Cancel, drop);
  CHECK((dropped == std::vector<std::uint32_t>{203}));

  // full queue
  ThrottleConfig small = c;
  small.queue_capacity = 2;
  Throttle u(small);
  CHECK(u.enqueue(ReqKind::New, 0, 1, 0) && u.enqueue(ReqKind::New, 0, 2, 0));
  CHECK(!u.enqueue(ReqKind::New, 0, 3, 0));
}

void test_amend_retarget() {
  OrderTableConfig oc;
  oc.max_requests = 1024;
  oc.max_orders = 256;
  oc.max_symbols = 4;
  OrderTable t(oc);
  const OrderSlot s = t.new_order(0, OrdSide::Buy, 10 * S, 100 * S);
  Report ack;
  ack.kind = ReportKind::Ack;
  ack.req_id = t.order(s).req_id;
  t.apply(ack);
  const std::uint32_t a = t.amend(s, 10 * S, 300 * S);
  CHECK(a != 0);
  CHECK(!t.retarget_amend(s, 10 * S, 200 * S));  // already on the wire
  t.mark_unsent(s);
  CHECK(t.retarget_amend(s, 10 * S, 200 * S));
  CHECK_EQ(t.position(0).open_buy_qty, 200 * S);
  CHECK_EQ(t.order(s).pending_req, a);  // same request, new parameters
  t.mark_sent(s);
  CHECK(!t.is_unsent(s));
  CHECK(t.order(s).pending == kPendingAmend);
}

}  // namespace

int main() {
  test_unlimited();
  test_priority_and_drain();
  test_expire_clear_remove();
  test_amend_retarget();
  return test_result("test_throttle");
}
