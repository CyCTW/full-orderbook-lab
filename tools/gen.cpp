// obl_gen: write a synthetic NYSE XDP Integrated Feed capture (pcap).
//
//   obl_gen --out data/synth.pcap [--messages 5e6] [--symbols 200] [--seed 42]
//           [--target-orders 1500] [--zipf 1.0] [--far 0.01] [--clear 0]
//           [--ab]   also write every packet on a B line (second multicast group)

#include <chrono>
#include <cstdio>

#include "args.hpp"
#include "obl/pcap/pcap.hpp"
#include "obl/sim/generator.hpp"

namespace {

constexpr std::uint32_t kLineA = 0xE0003B01;  // 224.0.59.1
constexpr std::uint32_t kLineB = 0xE0003C01;  // 224.0.60.1
constexpr std::uint32_t kSource = 0x0A000001;
constexpr std::uint16_t kPort = 11001;

struct PcapOut {
  obl::pcap::Writer& w;
  bool ab;
  std::uint64_t bytes = 0;
  void packet(std::uint64_t ts, const std::uint8_t* p, std::size_t n) {
    w.write_udp(ts, kSource, kLineA, kPort, kPort, p, n);
    if (ab) w.write_udp(ts + 3000, kSource + 1, kLineB, kPort, kPort, p, n);
    bytes += n;
  }
};

}  // namespace

int main(int argc, char** argv) {
  obl::tools::Args args(argc, argv);
  if (args.has("help")) {
    std::puts("usage: obl_gen --out FILE.pcap [--messages N] [--symbols N] [--seed N] [--target-orders N]\n"
              "               [--zipf S] [--far P] [--clear P] [--ab]");
    return 0;
  }
  obl::sim::GenConfig cfg;
  cfg.messages = args.u64("messages", cfg.messages);
  cfg.symbols = static_cast<std::uint32_t>(args.u64("symbols", cfg.symbols));
  cfg.seed = args.u64("seed", cfg.seed);
  cfg.target_orders = static_cast<std::uint32_t>(args.u64("target-orders", cfg.target_orders));
  cfg.zipf_s = args.num("zipf", cfg.zipf_s);
  cfg.far_order_prob = args.num("far", cfg.far_order_prob);
  cfg.symbol_clear_prob = args.num("clear", cfg.symbol_clear_prob);
  const std::string out = args.str("out", "synth.pcap");

  const auto t0 = std::chrono::steady_clock::now();
  obl::pcap::Writer writer(out);
  PcapOut sink{writer, args.has("ab")};
  obl::sim::PacketBuilder<PcapOut> packets(sink, cfg.max_msgs_per_packet, cfg.seed);
  obl::sim::FlowGenerator<obl::sim::PacketBuilder<PcapOut>> gen(cfg, packets);
  gen.run();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("wrote %s: %llu messages in %llu packets (%.1f MB payload), %llu live orders at end, %.1fs\n",
              out.c_str(), static_cast<unsigned long long>(cfg.messages),
              static_cast<unsigned long long>(packets.packets()), sink.bytes / 1e6,
              static_cast<unsigned long long>(gen.live_orders()), secs);
  return 0;
}
