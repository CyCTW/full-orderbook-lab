// Gateway send path: from the strategy calling new_order() to the order's bytes leaving.
//
//   decision -> send()   rdtsc before new_order() .. inside Transport::send (null transport) or
//                        right after ::send() returns (TCP loopback: the kernel has the bytes)
//   whole call           .. new_order() returns (adds bookkeeping done after the send)
// plus each stage alone. All risk checks are on; 20 own orders rest in the book (10 bids, 10
// asks) so the self-trade check has realistic work. Fenced rdtsc per call, timer overhead
// subtracted. Single-threaded except the TCP reader that drains the other end.
//
// usage: obl_gw_bench [--iters N] [--cpu C]

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "obl/gw/net/tcp.hpp"
#include "obl/gw/ocgc/gateway.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

constexpr std::int64_t S = kScale;
std::size_t g_iters = 200'000;

double tsc_ghz() {
  static const double ghz = [] {
    using Clock = std::chrono::steady_clock;
    const auto w0 = Clock::now();
    const auto c0 = __rdtsc();
    while (Clock::now() - w0 < std::chrono::milliseconds(200)) {
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - w0).count();
    return static_cast<double>(__rdtsc() - c0) / ns;
  }();
  return ghz;
}

void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

std::uint64_t timer_overhead() {
  std::vector<std::uint32_t> v(100'000);
  unsigned aux;
  for (auto& x : v) {
    _mm_lfence();
    const auto a = __rdtsc();
    _mm_lfence();
    const auto b = __rdtscp(&aux);
    _mm_lfence();
    x = static_cast<std::uint32_t>(b - a);
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct Dist {
  std::vector<std::uint32_t> c;
  void add(std::uint64_t cycles) { c.push_back(static_cast<std::uint32_t>(cycles)); }
  void print(const char* name, std::uint64_t oh) {
    std::sort(c.begin(), c.end());
    auto q = [&](double p) {
      const double v = static_cast<double>(c[static_cast<std::size_t>(p * (c.size() - 1))]) - static_cast<double>(oh);
      return std::max(0.0, v) / tsc_ghz();
    };
    std::printf("  %-38s %8.1f %8.1f %8.1f %9.1f\n", name, q(0.5), q(0.99), q(0.999), q(0.9999));
  }
};

// --- transports ---------------------------------------------------------------------------------

struct NullTransport {
  std::uint64_t sent_tsc = 0;
  void send(const std::uint8_t*, std::size_t) { sent_tsc = __rdtsc(); }
  void close() {}
};

struct TcpTransport {
  int fd = -1;
  std::uint64_t sent_tsc = 0;
  void send(const std::uint8_t* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
      const ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
      if (w > 0) off += static_cast<std::size_t>(w);
    }
    sent_tsc = __rdtsc();
  }
  void close() {}
};

struct NoListener {
  void on_order_update(const OrderUpdate&, const Order&) {}
  void on_request_dropped(OrderSlot, ReqKind, DropReason) {}
  void on_session(bool, CloseReason) {}
  void on_kill_switch(KillReason) {}
  void on_mass_cancel(const MassCancelReport&) {}
};

// --- the setup ----------------------------------------------------------------------------------

template <class Tx>
struct Rig {
  using Gw = OcgcGateway<Tx, NoListener>;
  InstrumentTable ins;
  std::unique_ptr<OrderTable> orders;
  std::unique_ptr<RiskEngine> risk;
  NoListener app;
  std::unique_ptr<Gw> gw;
  SymbolIdx sym = 0;
  std::uint8_t buf[256];

  explicit Rig(Tx& tx) {
    sym = ins.add({"700", 100 * S, true, false, 400 * S});
    OrderTableConfig oc;
    oc.max_orders = static_cast<std::uint32_t>(g_iters + 100'000);
    oc.max_requests = 2 * oc.max_orders;
    oc.max_symbols = 16;
    orders = std::make_unique<OrderTable>(oc);
    risk = std::make_unique<RiskEngine>(ins, *orders, 16);
    SymbolLimits L;
    L.max_order_qty = 100'000 * S;
    L.max_order_notional = 50'000'000 * S;
    L.collar_bps = 500;
    L.max_long = 1'000'000 * S;
    L.max_short = 1'000'000 * S;
    risk->set_symbol_limits(sym, L);
    AccountLimits A;
    A.max_gross_open_notional = 1'000'000'000 * S;
    A.duplicate_window_ns = 1'000'000;
    A.self_trade_check = true;
    A.allow_short_sell = true;
    risk->set_account_limits(A);

    GatewayConfig g;
    g.session.comp_id = "CO99999901";
    g.session.encrypted_password = "x";
    g.session.store_reserve_bytes = (g_iters + 100'000) * 192;
    g.session.store_reserve_msgs = g_iters + 100'000;
    g.broker_id = "1234";
    g.bcan = "ABC123.2568";
    g.trade_date = 20261003;
    gw = std::make_unique<Gw>(g, tx, app, ins, *orders, *risk);
    gw->prepare_templates(sym);
    // log on: feed the session a Logon reply
    gw->on_connected(1);
    gw->on_bytes(buf, encode_logon_reply(buf, 1, "OCGC", 2), 1);
    // resting orders: 10 bids at 380-389, 10 asks at 401-410 (the timed buys are at 390-399)
    for (int i = 0; i < 10; ++i) {
      ack(gw->new_order(sym, OrdSide::Buy, (380 + i) * S, 100 * S, 2).slot);
      ack(gw->new_order(sym, OrdSide::SellShort, (401 + i) * S, 100 * S, 2).slot);
    }
    if (orders->position(sym).live_orders != 20) {
      std::fprintf(stderr, "setup: expected 20 resting orders, have %u\n", orders->position(sym).live_orders);
      std::exit(1);
    }
  }

  // The exchange acknowledges / cancels (outside the timed region).
  void report(OrderSlot s, ReportKind k) {
    if (s == kNoOrder) return;
    Report r;
    r.kind = k;
    r.req_id = orders->order(s).req_id;
    orders->apply(r);
  }
  void ack(OrderSlot s) { report(s, ReportKind::Ack); }
  void cancel(OrderSlot s) { report(s, ReportKind::Cancelled); }
};

template <class Tx>
void run_end_to_end(const char* label, Tx& tx, std::uint64_t oh) {
  Rig<Tx> rig(tx);
  Dist to_send, whole;
  unsigned aux;
  std::uint64_t now = 1'000'000'000;
  const std::size_t warm = 20'000;
  for (std::size_t i = 0; i < g_iters + warm; ++i) {
    now += 10'000;  // 10 us apart
    const std::int64_t px = (390 + static_cast<std::int64_t>(i % 10)) * S;
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    const auto r = rig.gw->new_order(rig.sym, OrdSide::Buy, px, 100 * S, now);
    const auto t1 = __rdtscp(&aux);
    _mm_lfence();
    if (r.status != SendStatus::Sent) {
      std::fprintf(stderr, "order %zu not sent (status %d, risk %s)\n", i, static_cast<int>(r.status), to_string(r.risk));
      std::exit(1);
    }
    if (i >= warm) {
      to_send.add(tx.sent_tsc - t0);
      whole.add(t1 - t0);
    }
    rig.ack(r.slot);  // keep ~20 live orders: the exchange acks and the strategy cancels
    rig.cancel(r.slot);
  }
  std::printf("\n%s\n  %-38s %8s %8s %8s %9s\n", label, "ns", "p50", "p99", "p99.9", "p99.99");
  to_send.print("new_order() -> bytes handed over", oh);
  whole.print("new_order() whole call", oh);
}

template <class F>
void stage(const char* name, std::uint64_t oh, F&& f) {
  Dist d;
  unsigned aux;
  for (std::size_t i = 0; i < g_iters + 20'000; ++i) {
    _mm_lfence();
    const auto t0 = __rdtsc();
    _mm_lfence();
    f(i);
    const auto t1 = __rdtscp(&aux);
    _mm_lfence();
    if (i >= 20'000) d.add(t1 - t0);
  }
  d.print(name, oh);
}

void run_stages(std::uint64_t oh) {
  NullTransport tx;
  Rig<NullTransport> rig(tx);
  std::printf("\nstages alone (same setup)\n  %-38s %8s %8s %8s %9s\n", "ns", "p50", "p99", "p99.9", "p99.99");
  std::uint64_t now = 1'000'000'000;
  volatile int sink = 0;
  stage("risk.check_new (all checks)", oh, [&](std::size_t i) {
    now += 10'000;
    sink = static_cast<int>(rig.risk->check_new(rig.sym, OrdSide::Buy, (390 + static_cast<std::int64_t>(i % 10)) * S,
                                                100 * S, now));
  });
  AccountLimits A = rig.risk->account_limits();
  A.self_trade_check = false;
  rig.risk->set_account_limits(A);
  stage("risk.check_new (no self-trade scan)", oh, [&](std::size_t i) {
    now += 10'000;
    sink = static_cast<int>(rig.risk->check_new(rig.sym, OrdSide::Buy, (390 + static_cast<std::int64_t>(i % 10)) * S,
                                                100 * S, now));
  });
  std::vector<OrderSlot> slots;
  slots.reserve(g_iters + 20'000);
  stage("orders.new_order", oh, [&](std::size_t i) {
    slots.push_back(rig.orders->new_order(rig.sym, OrdSide::Buy, (390 + static_cast<std::int64_t>(i % 10)) * S, 100 * S));
  });
  for (OrderSlot s : slots) rig.cancel(s);
  Throttle thr;
  stage("throttle.admit (unlimited)", oh, [&](std::size_t) { sink = thr.admit(ReqKind::New, now); });
  ThrottleConfig tc;
  tc.msgs_per_sec = 1'000'000'000;
  Throttle thr2(tc);
  stage("throttle.admit (sliding window)", oh, [&](std::size_t i) { sink = thr2.admit(ReqKind::New, now + i * 2000); });
  NewOrderTemplate tpl({"CO99999901", "1234", "700", "ABC123.2568", WireSide::Buy, 20261003});
  stage("template fill_regs (encode + CRC)", oh, [&](std::size_t i) {
    tpl.fill_regs({static_cast<std::uint32_t>(i), 10'000'000 + static_cast<std::uint32_t>(i), 3'600'000'000ull + i,
                   400 * S, 100 * S});
  });
  MessageStore store;
  store.reserve((g_iters + 20'000) * 192, g_iters + 20'000, true);
  std::uint32_t seq = 1;
  stage("message store append (184 B)", oh, [&](std::size_t) { store.append(seq++, tpl.data(), tpl.size()); });
  (void)sink;
}

}  // namespace

int main(int argc, char** argv) {
  int cpu = 2;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--iters" && i + 1 < argc) g_iters = std::strtoull(argv[++i], nullptr, 10);
    if (a == "--cpu" && i + 1 < argc) cpu = std::atoi(argv[++i]);
  }
  pin(cpu);
  const std::uint64_t oh = timer_overhead();
  std::printf("tsc %.3f GHz, %zu orders per run, pinned to cpu %d\n", tsc_ghz(), g_iters, cpu);

  NullTransport null_tx;
  run_end_to_end("null transport (software only)", null_tx, oh);

  // TCP loopback: a reader thread on another CPU drains the far end
  net::TcpListener lst;
  lst.listen("127.0.0.1", 0);
  const int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a;
  net::make_addr("127.0.0.1", lst.port(), a);
  ::connect(cfd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  const int one = 1;
  setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int sfd = -1;
  while ((sfd = lst.accept()) < 0) {
  }
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    pin(cpu == 3 ? 2 : 3);
    std::vector<std::uint8_t> b(1 << 20);
    while (!stop.load(std::memory_order_relaxed))
      if (::recv(sfd, b.data(), b.size(), MSG_DONTWAIT) <= 0) _mm_pause();
  });
  TcpTransport tcp_tx{cfd};
  run_end_to_end("TCP loopback, kernel send() (bytes in the kernel)", tcp_tx, oh);
  stop.store(true);
  reader.join();
  ::close(cfd);
  ::close(sfd);

  run_stages(oh);
  return 0;
}
