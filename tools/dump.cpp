// obl_dump: inspect an XDP capture and sanity-check it against the book.
//
//   obl_dump FILE.pcap[.gz]... [--port P] [--key-by-group] [--print N] [--top K]
//            [--profile EVERY_N_MSGS [--cent PRICE_UNITS_PER_CENT]]
//
// Use this first on a real NYSE sample: message-type counts, short messages
// and "unknown order" counts quickly reveal a layout/version mismatch.

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "capture_util.hpp"
#include "obl/book/variants.hpp"
#include "obl/xdp/book_adapter.hpp"

namespace {

using namespace obl;

struct Printer : xdp::NullVisitor {
  std::uint64_t remaining;
  explicit Printer(std::uint64_t n) : remaining(n) {}
  bool take() { return remaining > 0 && remaining-- > 0; }
  static char side(Side s) { return s == Side::Buy ? 'B' : 'S'; }

  void on_sequence_reset(const xdp::SequenceNumberReset& m) {
    if (take()) std::printf("SeqReset   product=%u channel=%u\n", m.product_id, m.channel_id);
  }
  void on_symbol_mapping(const xdp::SymbolIndexMapping& m) {
    if (take())
      std::printf("SymbolMap  idx=%u sym=%.*s scale=%u mpv=%u lot=%u\n", m.symbol_index,
                  static_cast<int>(m.symbol_view().size()), m.symbol_view().data(), m.price_scale_code, m.mpv,
                  m.lot_size);
  }
  void on_symbol_clear(const xdp::SymbolClear& m) {
    if (take()) std::printf("SymClear   sym=%u\n", m.symbol_index);
  }
  void on_add(const xdp::AddOrder& m) {
    if (take())
      std::printf("Add        sym=%u seq=%u id=%" PRIu64 " %c %u @ %u\n", m.symbol_index, m.symbol_seq_num,
                  m.order_id, side(m.side), m.volume, m.price);
  }
  void on_modify(const xdp::ModifyOrder& m) {
    if (take())
      std::printf("Modify     sym=%u seq=%u id=%" PRIu64 " %c %u @ %u poschg=%u\n", m.symbol_index,
                  m.symbol_seq_num, m.order_id, side(m.side), m.volume, m.price, m.position_change);
  }
  void on_delete(const xdp::DeleteOrder& m) {
    if (take()) std::printf("Delete     sym=%u seq=%u id=%" PRIu64 "\n", m.symbol_index, m.symbol_seq_num, m.order_id);
  }
  void on_execution(const xdp::OrderExecution& m) {
    if (take())
      std::printf("Execution  sym=%u seq=%u id=%" PRIu64 " %u @ %u trade=%u\n", m.symbol_index, m.symbol_seq_num,
                  m.order_id, m.volume, m.price, m.trade_id);
  }
  void on_replace(const xdp::ReplaceOrder& m) {
    if (take())
      std::printf("Replace    sym=%u seq=%u id=%" PRIu64 " -> %" PRIu64 " %c %u @ %u\n", m.symbol_index,
                  m.symbol_seq_num, m.order_id, m.new_order_id, side(m.side), m.volume, m.price);
  }
  void on_other(std::uint16_t type, const std::uint8_t*, std::uint16_t size) {
    if (take()) std::printf("%-10s type=%u size=%u\n", std::string(xdp::msg_type_name(type)).c_str(), type, size);
  }
};

// Book adapter that periodically snapshots shape statistics (for calibrating
// the generator and sanity-checking the book) and histograms where adds land
// relative to the same-side best price.
struct Profiler : xdp::BookAdapter<book::MapStdBook> {
  using Base = xdp::BookAdapter<book::MapStdBook>;
  book::MapStdBook& b;
  std::uint64_t every, n = 0;
  Price unit;  // price units per cent (scale 6 => 10000)
  std::uint64_t dist_hist[9] = {};  // improve, 0, 1, 2, 3-5, 6-10, 11-50, 51-200, >200 (cents from same-side best)
  std::uint64_t no_best = 0;

  Profiler(book::MapStdBook& bk, std::uint64_t k, Price u, xdp::TickPolicy tick) : Base(bk, tick), b(bk), every(k), unit(u) {
    std::printf("%10s %9s %7s %13s %13s %15s %8s %7s\n", "msgs", "orders", "syms", "lvls/side avg", "lvls/side max",
                "best-lvl orders", "crossed", "locked");
  }

  void tick() {
    if (++n % every) return;
    std::size_t syms = 0, sides = 0, lv_sum = 0, lv_max = 0, crossed = 0, locked = 0;
    std::vector<std::uint32_t> at_best;
    for (SymbolId s = 0; s < b.symbols(); ++s) {
      const auto* sb = b.book(s);
      const auto* bid = sb->bids.best();
      const auto* ask = sb->asks.best();
      if (!bid && !ask) continue;
      ++syms;
      for (std::size_t lv : {sb->bids.size(), sb->asks.size()}) {
        if (!lv) continue;
        ++sides;
        lv_sum += lv;
        lv_max = std::max(lv_max, lv);
      }
      if (bid) at_best.push_back(bid->count);
      if (ask) at_best.push_back(ask->count);
      if (bid && ask && bid->price > ask->price) ++crossed;
      if (bid && ask && bid->price == ask->price) ++locked;
    }
    std::sort(at_best.begin(), at_best.end());
    const auto q = [&](double f) { return at_best.empty() ? 0u : at_best[std::min(at_best.size() - 1, std::size_t(f * at_best.size()))]; };
    char best[64];
    std::snprintf(best, sizeof(best), "p50 %u p99 %u max %u", q(0.5), q(0.99), at_best.empty() ? 0u : at_best.back());
    std::printf("%10llu %9zu %7zu %13.1f %13zu %15s %8zu %7zu\n", static_cast<unsigned long long>(n), b.order_count(), syms,
                sides ? double(lv_sum) / sides : 0.0, lv_max, best, crossed, locked);
  }

  void on_add(const xdp::AddOrder& m) {
    const auto* sb = b.book(m.symbol_index);
    const book::Level* best = !sb ? nullptr : (m.side == Side::Buy ? sb->bids.best() : sb->asks.best());
    if (!best) {
      ++no_best;
    } else {
      const Price d = (m.side == Side::Buy ? best->price - Price(m.price) : Price(m.price) - best->price) / unit;
      const int bucket = d < 0 ? 0 : d == 0 ? 1 : d == 1 ? 2 : d == 2 ? 3 : d <= 5 ? 4 : d <= 10 ? 5 : d <= 50 ? 6 : d <= 200 ? 7 : 8;
      ++dist_hist[bucket];
    }
    Base::on_add(m);
    tick();
  }
  void on_modify(const xdp::ModifyOrder& m) { Base::on_modify(m); tick(); }
  void on_delete(const xdp::DeleteOrder& m) { Base::on_delete(m); tick(); }
  void on_execution(const xdp::OrderExecution& m) { Base::on_execution(m); tick(); }
  void on_replace(const xdp::ReplaceOrder& m) { Base::on_replace(m); tick(); }

  void print_hist() const {
    static const char* names[9] = {"inside", "at best", "1c", "2c", "3-5c", "6-10c", "11-50c", "51-200c", ">200c"};
    std::uint64_t total = no_best;
    for (auto v : dist_hist) total += v;
    std::printf("\nadd price vs same-side best (cents away; 'inside' = improves the best):\n");
    for (int i = 0; i < 9; ++i) std::printf("  %-8s %6.2f%%\n", names[i], total ? 100.0 * dist_hist[i] / total : 0.0);
    std::printf("  %-8s %6.2f%%\n", "no best", total ? 100.0 * no_best / total : 0.0);
  }
};

}  // namespace

int main(int argc, char** argv) {
  tools::Args args(argc, argv);
  if (args.positional().empty()) {
    std::puts("usage: obl_dump FILE.pcap[.gz]... [--port P] [--key-by-group] [--print N] [--top K]\n"
              "                [--profile N [--cent UNITS]]");
    return 1;
  }
  const tools::FeedOptions opt(args);
  const pcap::Capture cap = tools::load_capture(args.positional());

  if (const auto n = args.u64("print", 0)) {
    Printer p(n);
    tools::decode_capture(cap, opt, p);
  }

  book::MapStdBook book;
  xdp::DecodeStats st;
  if (const auto every = args.u64("profile", 0)) {
    Profiler prof(book, every, static_cast<Price>(args.u64("cent", 10000)), opt.tick);
    st = tools::decode_capture(cap, opt, prof);
    prof.print_hist();
  } else {
    xdp::BookAdapter<book::MapStdBook> adapter(book, opt.tick);
    st = tools::decode_capture(cap, opt, adapter);
  }
  tools::print_decode_stats(st);

  std::puts("\nmessage types:");
  for (unsigned t = 0; t < st.by_type.size(); ++t)
    if (st.by_type[t])
      std::printf("  %3u %-24s %12llu\n", t, std::string(xdp::msg_type_name(static_cast<std::uint16_t>(t))).c_str(),
                  static_cast<unsigned long long>(st.by_type[t]));

  const auto& bs = book.stats();
  std::printf("\nbook: %zu live orders, unknown-order refs %llu, duplicate adds %llu, over-executions %llu, "
              "level inconsistencies %llu\n",
              book.order_count(), static_cast<unsigned long long>(bs.unknown_order),
              static_cast<unsigned long long>(bs.duplicate_add), static_cast<unsigned long long>(bs.over_execution),
              static_cast<unsigned long long>(bs.level_missing));
  if (bs.unknown_order)
    std::puts("  note: unknown-order refs are expected when a capture starts mid-session (no refresh replay)");

  std::size_t crossed = 0, locked = 0;
  for (SymbolId s = 0; s < book.symbols(); ++s) {
    const auto* b = book.book(s);
    const auto* bid = b->bids.best();
    const auto* ask = b->asks.best();
    if (bid && ask) {
      if (bid->price > ask->price) ++crossed;
      else if (bid->price == ask->price) ++locked;
    }
  }
  std::printf("final book: %zu crossed, %zu locked symbols\n", crossed, locked);

  struct Row {
    SymbolId sym;
    std::size_t bid_levels, ask_levels;
  };
  std::vector<Row> rows;
  for (SymbolId s = 0; s < book.symbols(); ++s) {
    const auto* b = book.book(s);
    if (b->bids.size() + b->asks.size()) rows.push_back({s, b->bids.size(), b->asks.size()});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.bid_levels + a.ask_levels > b.bid_levels + b.ask_levels;
  });
  const std::size_t top = std::min<std::size_t>(rows.size(), args.u64("top", 5));
  std::printf("\n%zu symbols with resting orders; deepest %zu:\n", rows.size(), top);
  for (std::size_t i = 0; i < top; ++i) {
    const auto& r = rows[i];
    const auto bid = book.depth(r.sym, Side::Buy, 1), ask = book.depth(r.sym, Side::Sell, 1);
    std::printf("  sym %-6u levels %5zu/%-5zu  best %10lld x %-8llu | %10lld x %-8llu\n", r.sym, r.bid_levels,
                r.ask_levels, bid.empty() ? 0LL : static_cast<long long>(bid[0].price),
                bid.empty() ? 0ULL : static_cast<unsigned long long>(bid[0].qty),
                ask.empty() ? 0LL : static_cast<long long>(ask[0].price),
                ask.empty() ? 0ULL : static_cast<unsigned long long>(ask[0].qty));
  }
  return 0;
}
