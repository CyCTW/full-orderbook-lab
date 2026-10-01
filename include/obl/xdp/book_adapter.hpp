#pragma once
// Routes decoded XDP messages into an L3Book.

#include "obl/book/l3_book.hpp"
#include "obl/xdp/decoder.hpp"

namespace obl::xdp {

template <class Book>
class BookAdapter : public NullVisitor {
 public:
  explicit BookAdapter(Book& book, bool tick_from_mpv = true) : book_(book), tick_from_mpv_(tick_from_mpv) {}

  void on_symbol_mapping(const SymbolIndexMapping& m) {
    if (tick_from_mpv_ && m.mpv > 0) book_.set_tick(m.symbol_index, m.mpv);
  }
  void on_symbol_clear(const SymbolClear& m) { book_.clear_symbol(m.symbol_index); }
  void on_add(const AddOrder& m) { book_.add(m.symbol_index, m.order_id, m.side, m.price, m.volume); }
  void on_modify(const ModifyOrder& m) {
    book_.modify(m.symbol_index, m.order_id, m.price, m.volume, m.position_change == 0);
  }
  void on_delete(const DeleteOrder& m) { book_.remove(m.symbol_index, m.order_id); }
  void on_execution(const OrderExecution& m) { book_.execute(m.symbol_index, m.order_id, m.volume); }
  void on_replace(const ReplaceOrder& m) {
    if (m.side_present) book_.replace(m.symbol_index, m.order_id, m.new_order_id, m.side, m.price, m.volume);
    else book_.replace(m.symbol_index, m.order_id, m.new_order_id, m.price, m.volume);
  }

 private:
  Book& book_;
  bool tick_from_mpv_;
};

}  // namespace obl::xdp
