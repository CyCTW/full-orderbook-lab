#pragma once
// Feed-neutral book events. The benchmark decodes a capture once into this
// form so data-structure timings exclude wire decoding and pcap handling.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "obl/common.hpp"

namespace obl::book {

// Cancel = partial cancel (ITCH 'X'): reduces the order like a fill, no trade.
enum class EventType : std::uint8_t { Add, Modify, Execute, Remove, Replace, Cancel, ClearSymbol, SetTick };
inline constexpr int kOrderEventTypes = 6;  // Add .. Cancel
inline constexpr const char* kEventTypeNames[] = {"add", "modify", "execute", "remove", "replace", "cancel"};

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
    case EventType::Execute:
    case EventType::Cancel: b.execute(e.sym, e.id, e.qty); break;
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
         e.type == EventType::Replace || e.type == EventType::Cancel;
}

// Raw event files (".ev"): an 8-byte magic, the event count, then the array.
// Same-machine scratch format, not a portable interchange format.
inline constexpr char kEventFileMagic[8] = {'O', 'B', 'L', 'E', 'V', '0', '0', '2'};

inline void save_events(const std::string& path, const std::vector<Event>& ev) {
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "wb"), &std::fclose);
  if (!f) throw std::runtime_error("cannot create " + path);
  const std::uint64_t n = ev.size();
  if (std::fwrite(kEventFileMagic, 1, 8, f.get()) != 8 || std::fwrite(&n, sizeof(n), 1, f.get()) != 1 ||
      std::fwrite(ev.data(), sizeof(Event), ev.size(), f.get()) != ev.size())
    throw std::runtime_error("write error on " + path);
}

// Packet boundaries for an event file (".pkt"): number of consecutive events
// in each packet, in order. Lets a replay reproduce how messages arrived in bursts.
inline constexpr char kPacketFileMagic[8] = {'O', 'B', 'L', 'P', 'K', 'T', '0', '1'};

inline void save_packets(const std::string& path, const std::vector<std::uint32_t>& sizes) {
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "wb"), &std::fclose);
  if (!f) throw std::runtime_error("cannot create " + path);
  const std::uint64_t n = sizes.size();
  if (std::fwrite(kPacketFileMagic, 1, 8, f.get()) != 8 || std::fwrite(&n, sizeof(n), 1, f.get()) != 1 ||
      std::fwrite(sizes.data(), sizeof(std::uint32_t), sizes.size(), f.get()) != sizes.size())
    throw std::runtime_error("write error on " + path);
}

inline std::vector<std::uint32_t> load_packets(const std::string& path) {
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!f) throw std::runtime_error("cannot open " + path);
  char magic[8];
  std::uint64_t n = 0;
  if (std::fread(magic, 1, 8, f.get()) != 8 || std::memcmp(magic, kPacketFileMagic, 8) != 0 ||
      std::fread(&n, sizeof(n), 1, f.get()) != 1)
    throw std::runtime_error("not a packet file: " + path);
  std::vector<std::uint32_t> sizes(n);
  if (std::fread(sizes.data(), sizeof(std::uint32_t), n, f.get()) != n) throw std::runtime_error("short read on " + path);
  return sizes;
}

inline std::vector<Event> load_events(const std::string& path) {
  std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!f) throw std::runtime_error("cannot open " + path);
  char magic[8];
  std::uint64_t n = 0;
  if (std::fread(magic, 1, 8, f.get()) != 8 || std::memcmp(magic, kEventFileMagic, 8) != 0 ||
      std::fread(&n, sizeof(n), 1, f.get()) != 1)
    throw std::runtime_error("not an event file (or old version): " + path);
  std::vector<Event> ev(n);
  if (std::fread(ev.data(), sizeof(Event), n, f.get()) != n) throw std::runtime_error("short read on " + path);
  return ev;
}

}  // namespace obl::book
