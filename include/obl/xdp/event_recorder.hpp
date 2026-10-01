#pragma once
// XDP visitor that records feed-neutral book events.

#include <vector>

#include "obl/book/events.hpp"
#include "obl/xdp/decoder.hpp"

namespace obl::xdp {

class EventRecorder : public NullVisitor {
 public:
  using Event = book::Event;
  using T = book::EventType;

  explicit EventRecorder(std::vector<Event>& out, TickPolicy tick = TickPolicy::Cent) : out_(out), tick_(tick) {}

  void on_symbol_mapping(const SymbolIndexMapping& m) {
    if (const Price t = tick_for(m, tick_); t > 0)
      out_.push_back({T::SetTick, Side::Buy, false, true, m.symbol_index, 0, 0, t, 0});
  }
  void on_symbol_clear(const SymbolClear& m) {
    out_.push_back({T::ClearSymbol, Side::Buy, false, true, m.symbol_index, 0, 0, 0, 0});
  }
  void on_add(const AddOrder& m) {
    out_.push_back({T::Add, m.side, false, true, m.symbol_index, m.order_id, 0, m.price, m.volume});
  }
  void on_modify(const ModifyOrder& m) {
    out_.push_back({T::Modify, m.side, m.position_change == 0, true, m.symbol_index, m.order_id, 0, m.price, m.volume});
  }
  void on_delete(const DeleteOrder& m) {
    out_.push_back({T::Remove, Side::Buy, false, true, m.symbol_index, m.order_id, 0, 0, 0});
  }
  void on_execution(const OrderExecution& m) {
    out_.push_back({T::Execute, Side::Buy, false, true, m.symbol_index, m.order_id, 0, m.price, m.volume});
  }
  void on_replace(const ReplaceOrder& m) {
    out_.push_back({T::Replace, m.side, false, m.side_present, m.symbol_index, m.order_id, m.new_order_id, m.price, m.volume});
  }

 private:
  std::vector<Event>& out_;
  TickPolicy tick_;
};

}  // namespace obl::xdp
