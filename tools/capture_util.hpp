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
  xdp::TickPolicy tick = xdp::TickPolicy::Cent;

  explicit FeedOptions(const Args& a) {
    port = static_cast<int>(a.num("port", -1));
    key_by_group = a.has("key-by-group");
    const std::string t = a.str("tick", "cent");
    tick = t == "mpv" ? xdp::TickPolicy::Mpv : t == "none" ? xdp::TickPolicy::None : xdp::TickPolicy::Cent;
  }

  bool accept(const pcap::Datagram& d) const { return port < 0 || d.dst_port == port; }
};

// Loads one or more captures and concatenates them in the given order
// (e.g. the hourly files of one session).
inline pcap::Capture load_capture(const std::vector<std::string>& paths) {
  const auto t0 = std::chrono::steady_clock::now();
  pcap::Capture cap;
  for (const auto& path : paths) {
    pcap::Capture part = pcap::load(path);
    if (cap.bytes.empty()) {
      cap = std::move(part);
      continue;
    }
    const std::size_t base = cap.bytes.size();
    if (base + part.bytes.size() > 0xFFFFFFFFull) throw std::runtime_error("combined captures exceed 4 GiB");
    cap.bytes.insert(cap.bytes.end(), part.bytes.begin(), part.bytes.end());
    for (auto d : part.datagrams) {
      d.offset += static_cast<std::uint32_t>(base);
      cap.datagrams.push_back(d);
    }
    cap.frames += part.frames;
    cap.non_udp += part.non_udp;
    cap.truncated += part.truncated;
  }
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("capture %s%s: %.1f MB, %llu frames, %llu UDP datagrams, %llu non-UDP, %llu truncated (%.2fs)\n",
              paths.empty() ? "" : paths[0].c_str(), paths.size() > 1 ? (" (+" + std::to_string(paths.size() - 1) + " more)").c_str() : "",
              cap.bytes.size() / 1e6, static_cast<unsigned long long>(cap.frames),
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
