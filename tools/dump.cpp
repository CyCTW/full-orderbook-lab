// obl_dump: inspect an XDP capture and sanity-check it against the book.
//
//   obl_dump FILE.pcap[.gz] [--port P] [--key-by-group] [--print N] [--top K]
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

}  // namespace

int main(int argc, char** argv) {
  tools::Args args(argc, argv);
  if (args.positional().empty()) {
    std::puts("usage: obl_dump FILE.pcap[.gz] [--port P] [--key-by-group] [--print N] [--top K]");
    return 1;
  }
  const tools::FeedOptions opt(args);
  const pcap::Capture cap = tools::load_capture(args.positional()[0]);

  if (const auto n = args.u64("print", 0)) {
    Printer p(n);
    tools::decode_capture(cap, opt, p);
  }

  book::MapStdBook book;
  xdp::BookAdapter<book::MapStdBook> adapter(book, opt.tick_from_mpv);
  const auto st = tools::decode_capture(cap, opt, adapter);
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
