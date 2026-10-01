// obl_bench: compare L3 book data structures on a captured / synthetic feed.
//
//   obl_bench FILE.pcap[.gz] [--repeat 3] [--latency] [--only SUBSTR] [--reserve N]
//                            [--port P] [--key-by-group] [--no-mpv-tick] [--no-fork]
//
// The capture is decoded once into feed-neutral events; each variant then
// replays the same events. Every variant runs in a forked child so it starts
// from a clean heap and its RSS growth can be measured in isolation. All
// variants must end with the same full-depth checksum (including queue order).

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "capture_util.hpp"
#include "obl/book/events.hpp"
#include "obl/book/variants.hpp"
#include "obl/xdp/book_adapter.hpp"

namespace {

using namespace obl;
using Clock = std::chrono::steady_clock;

struct Result {
  double best_ns = 0, median_ns = 0;      // per event
  double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;  // latency ns (if measured)
  double e2e_ns = 0;                      // decode + book per message, from raw packets
  std::uint64_t checksum = 0;
  std::uint64_t live_orders = 0;
  std::uint64_t unknown = 0, level_missing = 0;
  double rss_mb = 0;
  bool ok = false;
};

long rss_kb() {
  std::ifstream f("/proc/self/statm");
  long pages = 0, resident = 0;
  f >> pages >> resident;
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

double tsc_ghz() {
  static double ghz = [] {
    const auto t0 = Clock::now();
    const auto c0 = __rdtsc();
    while (Clock::now() - t0 < std::chrono::milliseconds(100)) {
    }
    const auto c1 = __rdtsc();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    return (c1 - c0) / ns;
  }();
  return ghz;
}

template <class Book>
Result run_variant(const std::vector<book::Event>& events, const pcap::Capture& cap,
                   const tools::FeedOptions& opt, int repeat, bool latency, std::size_t reserve) {
  Result r;
  const long rss0 = rss_kb();
  std::vector<double> runs;
  for (int i = 0; i < repeat; ++i) {
    auto b = std::make_unique<Book>();
    if (reserve) b->reserve(reserve, 1 << 16);
    const auto t0 = Clock::now();
    for (const auto& e : events) book::apply(*b, e);
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    runs.push_back(ns / std::max<std::size_t>(1, events.size()));
    if (i == repeat - 1) {
      r.checksum = b->checksum();
      r.live_orders = b->order_count();
      r.unknown = b->stats().unknown_order;
      r.level_missing = b->stats().level_missing;
      r.rss_mb = (rss_kb() - rss0) / 1024.0;
    }
  }
  std::sort(runs.begin(), runs.end());
  r.best_ns = runs.front();
  r.median_ns = runs[runs.size() / 2];

  if (latency) {
    std::vector<std::uint32_t> cycles(events.size());
    auto b = std::make_unique<Book>();
    if (reserve) b->reserve(reserve, 1 << 16);
    for (std::size_t i = 0; i < events.size(); ++i) {
      const auto c0 = __rdtsc();
      book::apply(*b, events[i]);
      const auto c1 = __rdtsc();
      cycles[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(c1 - c0, 0xFFFFFFFFu));
    }
    std::sort(cycles.begin(), cycles.end());
    const double g = tsc_ghz();
    const auto pct = [&](double q) { return cycles[std::min(cycles.size() - 1, std::size_t(q * cycles.size()))] / g; };
    if (!cycles.empty()) {
      r.p50 = pct(0.5);
      r.p90 = pct(0.9);
      r.p99 = pct(0.99);
      r.p999 = pct(0.999);
      r.max = cycles.back() / g;
    }
  }

  {  // end to end: arbitration + decode + book, straight from the packets
    auto b = std::make_unique<Book>();
    if (reserve) b->reserve(reserve, 1 << 16);
    xdp::BookAdapter<Book> adapter(*b, opt.tick_from_mpv);
    const auto t0 = Clock::now();
    const auto st = tools::decode_capture(cap, opt, adapter);
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    r.e2e_ns = ns / std::max<std::uint64_t>(1, st.messages);
    if (b->checksum() != r.checksum) r.level_missing += 1'000'000;  // flags a decoder/replay divergence
  }
  r.ok = true;
  return r;
}

template <class F>
Result in_child(bool fork_enabled, F&& f) {
  if (!fork_enabled) return f();
  int fds[2];
  if (pipe(fds) != 0) return f();
  const pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    const Result r = f();
    [[maybe_unused]] auto n = write(fds[1], &r, sizeof(r));
    _exit(0);
  }
  close(fds[1]);
  Result r;
  if (read(fds[0], &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) r.ok = false;
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  tools::Args args(argc, argv);
  if (args.positional().empty()) {
    std::puts("usage: obl_bench FILE.pcap[.gz] [--repeat N] [--latency] [--only SUBSTR] [--reserve N]\n"
              "                 [--port P] [--key-by-group] [--no-mpv-tick] [--no-fork]");
    return 1;
  }
  const tools::FeedOptions opt(args);
  const int repeat = static_cast<int>(std::max<std::uint64_t>(1, args.u64("repeat", 3)));
  const bool latency = args.has("latency");
  const std::string only = args.str("only", "");
  const std::size_t reserve = args.u64("reserve", 0);
  const bool fork_enabled = !args.has("no-fork");

  const pcap::Capture cap = tools::load_capture(args.positional()[0]);

  std::vector<book::Event> events;
  events.reserve(cap.datagrams.size() * 8);
  xdp::EventRecorder rec(events, opt.tick_from_mpv);
  const auto t0 = Clock::now();
  const auto st = tools::decode_capture(cap, opt, rec);
  const double decode_ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
  tools::print_decode_stats(st);
  std::size_t counts[8] = {};
  for (const auto& e : events) ++counts[static_cast<int>(e.type)];
  std::printf("events %zu: add %zu, modify %zu, execute %zu, remove %zu, replace %zu, clear %zu  "
              "(decode+record %.1f ns/msg)\n\n",
              events.size(), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
              decode_ns / std::max<std::uint64_t>(1, st.messages));

  std::printf("%-44s %9s %9s %8s %9s", "variant", "best", "median", "Mevt/s", "e2e/msg");
  if (latency) std::printf(" %7s %7s %7s %7s %8s", "p50", "p90", "p99", "p99.9", "max");
  std::printf(" %8s %9s  %s\n", "dRSS MB", "orders", "checksum");

  std::uint64_t ref = 0;
  bool have_ref = false, mismatch = false;
  book::for_each_variant([&]<class Book>() {
    const std::string name = Book::name();
    if (!only.empty() && name.find(only) == std::string::npos) return;
    const Result r = in_child(fork_enabled, [&] {
      return run_variant<Book>(events, cap, opt, repeat, latency, reserve);
    });
    if (!r.ok) {
      std::printf("%-44s  FAILED (child crashed)\n", name.c_str());
      mismatch = true;
      return;
    }
    if (!have_ref) {
      ref = r.checksum;
      have_ref = true;
    }
    const bool same = r.checksum == ref && r.level_missing == 0;
    mismatch |= !same;
    std::printf("%-44s %7.1fns %7.1fns %8.2f %7.1fns", name.c_str(), r.best_ns, r.median_ns, 1e3 / r.best_ns,
                r.e2e_ns);
    if (latency) std::printf(" %7.0f %7.0f %7.0f %7.0f %8.0f", r.p50, r.p90, r.p99, r.p999, r.max);
    std::printf(" %8.1f %9" PRIu64 "  %016" PRIx64 "%s\n", r.rss_mb, r.live_orders, r.checksum,
                same ? "" : "  <-- MISMATCH");
  });
  if (mismatch) {
    std::puts("\nERROR: variants disagree on the final book state");
    return 2;
  }
  std::puts("\nall variants agree on the final book (full depth + queue order)");
  return 0;
}
