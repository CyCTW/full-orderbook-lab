// obl_itch: Nasdaq TotalView-ITCH 5.0 sample files -> book events.
//
//   obl_itch FILE.gz scan [--top 30]
//       message counts per type, order-message counts per symbol (top N)
//   obl_itch FILE.gz extract (--top N | --symbols AAPL,NVDA,...) --out OUT.ev [--profile EVERY]
//       writes feed-neutral events for the chosen symbols (a full day of the
//       whole market does not fit in memory), then optionally replays them
//       and prints the book shape every EVERY events.
//       Also writes OUT.pkt: approximate packet boundaries. The sample files
//       carry no MoldUDP64 framing, so packets are rebuilt from the full
//       message stream: consecutive messages with the same matching-engine
//       timestamp share a packet, up to the MoldUDP64 payload limit. Only the
//       chosen symbols' messages are kept inside each packet.
//
// Symbols are identified by ITCH stock locate. Order reference numbers are
// unique per day, so (locate, ref) is a valid book key.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "args.hpp"
#include "obl/book/events.hpp"
#include "obl/book/variants.hpp"
#include "obl/itch/itch.hpp"

namespace {

using namespace obl;
using book::Event;
using T = book::EventType;
using Clock = std::chrono::steady_clock;

struct Scan {
  std::uint64_t messages = 0, size_mismatch = 0, unknown_type = 0;
  std::uint64_t by_type[256] = {};
  std::vector<std::uint64_t> order_msgs = std::vector<std::uint64_t>(65536, 0);
  std::vector<std::string> names = std::vector<std::string>(65536);
};

bool is_order_msg(std::uint8_t t) {
  return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

Scan scan(const std::string& path) {
  Scan s;
  itch::FileReader r(path);
  const std::uint8_t* m;
  std::uint16_t len;
  const auto t0 = Clock::now();
  while (r.next(m, len)) {
    ++s.messages;
    const std::uint8_t t = m[0];
    ++s.by_type[t];
    const std::uint16_t want = itch::expected_size(t);
    if (want == 0) ++s.unknown_type;
    else if (len != want) ++s.size_mismatch;
    if (t == 'R' && len >= 39) {
      const auto d = itch::decode_stock_directory(m);
      s.names[d.locate] = d.symbol();
    } else if (is_order_msg(t) && len >= 3) {
      ++s.order_msgs[itch::locate(m)];
    }
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("%s: %llu messages, %.1f GB decompressed, %.1fs; size mismatches %llu, unknown types %llu, "
              "truncated %llu\n",
              path.c_str(), static_cast<unsigned long long>(s.messages), r.bytes() / 1e9, secs,
              static_cast<unsigned long long>(s.size_mismatch), static_cast<unsigned long long>(s.unknown_type),
              static_cast<unsigned long long>(r.truncated()));
  return s;
}

std::vector<std::uint16_t> top_symbols(const Scan& s, std::size_t n) {
  std::vector<std::uint16_t> loc;
  for (std::uint32_t i = 0; i < 65536; ++i)
    if (s.order_msgs[i]) loc.push_back(static_cast<std::uint16_t>(i));
  std::sort(loc.begin(), loc.end(), [&](auto a, auto b) { return s.order_msgs[a] > s.order_msgs[b]; });
  if (loc.size() > n) loc.resize(n);
  return loc;
}

// Replays events through a reference-quality book and prints its shape.
void profile(const std::vector<Event>& ev, std::uint64_t every, Price cent) {
  book::MapStdBook b;
  std::uint64_t dist[9] = {}, no_best = 0;
  std::printf("%11s %9s %6s %13s %13s %22s %8s %7s\n", "events", "orders", "syms", "lvls/side avg",
              "lvls/side max", "best-lvl orders", "crossed", "locked");
  for (std::uint64_t n = 0; n < ev.size(); ++n) {
    const Event& e = ev[n];
    if (e.type == T::Add) {
      const auto* sb = b.book(e.sym);
      const book::Level* best = !sb ? nullptr : (e.side == Side::Buy ? sb->bids.best() : sb->asks.best());
      if (!best) {
        ++no_best;
      } else {
        const Price d = (e.side == Side::Buy ? best->price - e.price : e.price - best->price) / cent;
        ++dist[d < 0 ? 0 : d == 0 ? 1 : d == 1 ? 2 : d == 2 ? 3 : d <= 5 ? 4 : d <= 10 ? 5 : d <= 50 ? 6 : d <= 200 ? 7 : 8];
      }
    }
    book::apply(b, e);
    if ((n + 1) % every) continue;
    std::size_t syms = 0, sides = 0, lv = 0, lv_max = 0, crossed = 0, locked = 0;
    std::vector<std::uint32_t> at_best;
    for (SymbolId s = 0; s < b.symbols(); ++s) {
      const auto* sb = b.book(s);
      const auto* bid = sb->bids.best();
      const auto* ask = sb->asks.best();
      if (!bid && !ask) continue;
      ++syms;
      for (std::size_t k : {sb->bids.size(), sb->asks.size()})
        if (k) ++sides, lv += k, lv_max = std::max(lv_max, k);
      if (bid) at_best.push_back(bid->count);
      if (ask) at_best.push_back(ask->count);
      if (bid && ask && bid->price > ask->price) ++crossed;
      if (bid && ask && bid->price == ask->price) ++locked;
    }
    std::sort(at_best.begin(), at_best.end());
    const auto q = [&](double f) {
      return at_best.empty() ? 0u : at_best[std::min(at_best.size() - 1, std::size_t(f * at_best.size()))];
    };
    char best[64];
    std::snprintf(best, sizeof(best), "p50 %u p99 %u max %u", q(0.5), q(0.99), at_best.empty() ? 0u : at_best.back());
    std::printf("%11llu %9zu %6zu %13.1f %13zu %22s %8zu %7zu\n", static_cast<unsigned long long>(n + 1),
                b.order_count(), syms, sides ? double(lv) / sides : 0.0, lv_max, best, crossed, locked);
  }
  static const char* names[9] = {"inside", "at best", "1c", "2c", "3-5c", "6-10c", "11-50c", "51-200c", ">200c"};
  std::uint64_t total = no_best;
  for (auto v : dist) total += v;
  std::printf("\nadd price vs same-side best (cents away; 'inside' = improves the best):\n");
  for (int i = 0; i < 9; ++i) std::printf("  %-8s %6.2f%%\n", names[i], total ? 100.0 * dist[i] / total : 0.0);
  std::printf("  %-8s %6.2f%%\n", "no best", total ? 100.0 * no_best / total : 0.0);
  const auto& st = b.stats();
  std::printf("\nbook: %zu live orders at end, unknown-order refs %llu, duplicate adds %llu, over-executions %llu, "
              "level inconsistencies %llu\n",
              b.order_count(), static_cast<unsigned long long>(st.unknown_order),
              static_cast<unsigned long long>(st.duplicate_add), static_cast<unsigned long long>(st.over_execution),
              static_cast<unsigned long long>(st.level_missing));
}

}  // namespace

int main(int argc, char** argv) {
  tools::Args args(argc, argv);
  const auto& pos = args.positional();
  if (pos.size() < 2 || (pos[1] != "scan" && pos[1] != "extract")) {
    std::puts("usage: obl_itch FILE.gz scan [--top N]\n"
              "       obl_itch FILE.gz extract (--top N | --symbols A,B,..) --out OUT.ev [--profile EVERY]");
    return 1;
  }
  const std::string path = pos[0];

  if (pos[1] == "scan") {
    const Scan s = scan(path);
    std::puts("\nmessage types:");
    for (int t = 0; t < 256; ++t)
      if (s.by_type[t]) std::printf("  '%c' %12llu\n", t, static_cast<unsigned long long>(s.by_type[t]));
    const auto top = top_symbols(s, args.u64("top", 30));
    std::uint64_t all = 0;
    for (auto v : s.order_msgs) all += v;
    std::printf("\ntop symbols by order messages (of %llu):\n", static_cast<unsigned long long>(all));
    for (auto l : top)
      std::printf("  %-8s locate %5u %12llu  %5.2f%%\n", s.names[l].c_str(), l,
                  static_cast<unsigned long long>(s.order_msgs[l]), 100.0 * s.order_msgs[l] / std::max<std::uint64_t>(1, all));
    return 0;
  }

  // ---- extract
  std::vector<bool> want(65536, false);
  std::vector<std::string> names(65536);
  if (args.has("symbols")) {
    std::set<std::string> wanted;
    std::stringstream ss(args.str("symbols", ""));
    for (std::string t; std::getline(ss, t, ',');) wanted.insert(t);
    // locates come from the stock directory at the start of the file
    itch::FileReader r(path);
    const std::uint8_t* m;
    std::uint16_t len;
    while (r.next(m, len)) {
      if (m[0] == 'R' && len >= 39) {
        const auto d = itch::decode_stock_directory(m);
        if (wanted.count(d.symbol())) want[d.locate] = true, names[d.locate] = d.symbol();
      } else if (is_order_msg(m[0])) {
        break;  // directory is complete before the first order message
      }
    }
  } else {
    const Scan s = scan(path);
    for (auto l : top_symbols(s, args.u64("top", 10))) want[l] = true, names[l] = s.names[l];
  }
  std::printf("extracting:");
  std::size_t nsel = 0;
  for (std::uint32_t i = 0; i < 65536; ++i)
    if (want[i]) std::printf(" %s", names[i].c_str()), ++nsel;
  std::printf(" (%zu symbols)\n", nsel);

  std::vector<Event> ev;
  ev.reserve(50'000'000);
  for (std::uint32_t i = 0; i < 65536; ++i)  // ITCH prices have 4 decimals: one cent = 100
    if (want[i]) ev.push_back({T::SetTick, Side::Buy, false, true, i, 0, 0, 100, 0});
  itch::FileReader r(path);
  const std::uint8_t* m;
  std::uint16_t len;
  const auto t0 = Clock::now();
  std::uint64_t short_msgs = 0;
  // packet reconstruction over the whole stream (all symbols, all types)
  constexpr std::size_t kMoldPayload = 1500 - 20 - 8 - 20;  // Ethernet MTU - IPv4 - UDP - MoldUDP64 header
  std::uint64_t pkt_id = 0, cur_ts = ~0ull;
  std::size_t cur_bytes = 0;
  std::vector<std::uint64_t> ev_pkt(ev.size(), 0);
  for (std::size_t i = 0; i < ev_pkt.size(); ++i) ev_pkt[i] = ~0ull - i;  // SetTick events: one "packet" each
  while (r.next(m, len)) {
    const std::uint8_t t = m[0];
    if (len >= 11) {
      const std::uint64_t ts = itch::timestamp(m);
      if (ts != cur_ts || cur_bytes + 2 + len > kMoldPayload) {
        ++pkt_id;
        cur_ts = ts;
        cur_bytes = 0;
      }
      cur_bytes += 2 + len;
    }
    if (!is_order_msg(t) || len < 3 || !want[itch::locate(m)]) continue;
    if (len < itch::expected_size(t)) {
      ++short_msgs;
      continue;
    }
    switch (t) {
      case 'A':
      case 'F': {
        const auto a = itch::decode_add(m);
        ev.push_back({T::Add, a.side, false, true, a.locate, a.ref, 0, a.price, a.shares});
        break;
      }
      case 'E':
      case 'C': {
        const auto x = itch::decode_executed(m);
        ev.push_back({T::Execute, Side::Buy, false, true, x.locate, x.ref, 0, 0, x.shares});
        break;
      }
      case 'X': {
        const auto x = itch::decode_cancel(m);
        ev.push_back({T::Cancel, Side::Buy, false, true, x.locate, x.ref, 0, 0, x.shares});
        break;
      }
      case 'D': {
        const auto x = itch::decode_delete(m);
        ev.push_back({T::Remove, Side::Buy, false, true, x.locate, x.ref, 0, 0, 0});
        break;
      }
      case 'U': {
        const auto x = itch::decode_replace(m);
        ev.push_back({T::Replace, Side::Buy, false, /*side_present=*/false, x.locate, x.orig_ref, x.new_ref, x.price,
                      x.shares});
        break;
      }
    }
    ev_pkt.resize(ev.size(), pkt_id);
  }
  std::printf("%zu events in %.1fs (short messages %llu)\n", ev.size(),
              std::chrono::duration<double>(Clock::now() - t0).count(), static_cast<unsigned long long>(short_msgs));
  const std::string out = args.str("out", "itch.ev");
  book::save_events(out, ev);
  std::printf("wrote %s (%.1f MB)\n", out.c_str(), ev.size() * sizeof(Event) / 1e6);
  {
    std::vector<std::uint32_t> sizes;
    for (std::size_t i = 0; i < ev.size(); ++i) {
      if (i == 0 || ev_pkt[i] != ev_pkt[i - 1]) sizes.push_back(0);
      ++sizes.back();
    }
    std::uint64_t hist[6] = {};  // 1, 2-4, 5-9, 10-19, 20-39, 40+
    std::uint32_t maxp = 0;
    for (auto n : sizes) {
      ++hist[n == 1 ? 0 : n < 5 ? 1 : n < 10 ? 2 : n < 20 ? 3 : n < 40 ? 4 : 5];
      maxp = std::max(maxp, n);
    }
    const std::string pkt = out.substr(0, out.rfind('.')) + ".pkt";
    book::save_packets(pkt, sizes);
    std::printf("wrote %s: %zu packets, avg %.2f events/packet, max %u; packets by size: 1:%llu 2-4:%llu 5-9:%llu "
                "10-19:%llu 20-39:%llu 40+:%llu\n",
                pkt.c_str(), sizes.size(), double(ev.size()) / sizes.size(), maxp,
                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5]);
  }
  if (const auto every = args.u64("profile", 0)) profile(ev, every, 100);
  return 0;
}
