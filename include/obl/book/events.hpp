#pragma once
// Feed-neutral book events. The benchmark decodes a capture once into this
// form so data-structure timings exclude wire decoding and pcap handling.

#include <cstdint>
#include <vector>

#include "obl/common.hpp"

namespace obl::book {

enum class EventType : std::uint8_t { Add, Modify, Execute, Remove, Replace, ClearSymbol, SetTick };

struct Event {
  EventType type;
  Side side;
  bool keep_priority;
  bool side_present;  // Replace: false = inherit the side of the replaced order
  SymbolId sym;
  OrderId id;
  OrderId new_id;
  Price price;
  Qty qty;
};

template <class Book>
inline void apply(Book& b, const Event& e) {
  switch (e.type) {
    case EventType::Add: b.add(e.sym, e.id, e.side, e.price, e.qty); break;
    case EventType::Modify: b.modify(e.sym, e.id, e.price, e.qty, e.keep_priority); break;
    case EventType::Execute: b.execute(e.sym, e.id, e.qty); break;
    case EventType::Remove: b.remove(e.sym, e.id); break;
    case EventType::Replace:
      if (e.side_present) b.replace(e.sym, e.id, e.new_id, e.side, e.price, e.qty);
      else b.replace(e.sym, e.id, e.new_id, e.price, e.qty);
      break;
    case EventType::ClearSymbol: b.clear_symbol(e.sym); break;
    case EventType::SetTick: b.set_tick(e.sym, e.price); break;
  }
}

// Events that reference an existing order id (stage-2 prefetch target).
inline bool references_order(const Event& e) {
  return e.type == EventType::Modify || e.type == EventType::Execute || e.type == EventType::Remove ||
         e.type == EventType::Replace;
}

}  // namespace obl::book
