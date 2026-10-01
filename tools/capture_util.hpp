#pragma once
// Shared capture loading / decoding for obl_dump and obl_bench.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "args.hpp"
#include "obl/pcap/pcap.hpp"
#include "obl/xdp/decoder.hpp"
#include "obl/xdp/event_recorder.hpp"

namespace obl::tools {

struct FeedOptions {
  int port = -1;              // only this UDP destination port (-1 = all)
  bool key_by_group = false;  // sequence-track per group:port instead of per port
  bool tick_from_mpv = true;

  explicit FeedOptions(const Args& a) {
    port = static_cast<int>(a.num("port", -1));
    key_by_group = a.has("key-by-group");
    tick_from_mpv = !a.has("no-mpv-tick");
  }

  bool accept(const pcap::Datagram& d) const { return port < 0 || d.dst_port == port; }
};

inline pcap::Capture load_capture(const std::string& path) {
  const auto t0 = std::chrono::steady_clock::now();
  pcap::Capture cap = pcap::load(path);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("capture %s: %.1f MB, %llu frames, %llu UDP datagrams, %llu non-UDP, %llu truncated (%.2fs)\n",
              path.c_str(), cap.bytes.size() / 1e6, static_cast<unsigned long long>(cap.frames),
              static_cast<unsigned long long>(cap.datagrams.size()),
              static_cast<unsigned long long>(cap.non_udp), static_cast<unsigned long long>(cap.truncated), s);
  return cap;
}

// Runs every accepted datagram through sequence arbitration + decoding.
template <class Visitor>
xdp::DecodeStats decode_capture(const pcap::Capture& cap, const FeedOptions& opt, Visitor& v) {
  xdp::DecodeStats st;
  xdp::SequenceTracker seq;
  for (const auto& d : cap.datagrams) {
    if (!opt.accept(d)) continue;
    xdp::process_datagram(seq, d.stream_key(opt.key_by_group), cap.payload(d), d.length, v, st);
  }
  return st;
}

inline void print_decode_stats(const xdp::DecodeStats& st) {
  std::printf("packets %llu (heartbeats %llu, duplicates %llu, malformed %llu), messages %llu, "
              "gaps %llu (%llu msgs missing), short msgs %llu\n",
              static_cast<unsigned long long>(st.packets), static_cast<unsigned long long>(st.heartbeats),
              static_cast<unsigned long long>(st.duplicate_packets),
              static_cast<unsigned long long>(st.malformed_packets), static_cast<unsigned long long>(st.messages),
              static_cast<unsigned long long>(st.gaps), static_cast<unsigned long long>(st.missing_messages),
              static_cast<unsigned long long>(st.short_messages));
}

}  // namespace obl::tools
