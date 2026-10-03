#pragma once

// End-of-day reconciliation: our fills, rebuilt from the audit log, against another record of
// the day's trades (drop copy, clearing or broker file) keyed by Execution ID.
//
// Fills from the audit log: inbound Execution Reports with Exec Type Trade, deduplicated by
// Execution ID (PossDup / PossResend replays carry the same ID); Trade Cancel reports (Exec
// Type H) remove the fill they reference.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "obl/gw/audit.hpp"
#include "obl/gw/ocgc/messages.hpp"

namespace obl::gw::ocgc {

struct Fill {
  std::string exec_id, cl_ord_id, order_id, security_id;
  std::uint8_t side = 0;  // 1 buy, 2 sell
  std::int64_t qty = 0, price = 0;
  std::uint64_t ts = 0;
};

inline bool operator==(const Fill& a, const Fill& b) {
  return a.exec_id == b.exec_id && a.security_id == b.security_id && a.side == b.side && a.qty == b.qty &&
         a.price == b.price;
}

inline std::vector<Fill> fills_from_audit(const std::string& path, bool* ok = nullptr) {
  std::map<std::string, Fill> by_exec;
  std::vector<std::string> order;
  const bool read = read_audit(path, [&](const AuditRecord& r) {
    if (r.dir != AuditDir::In || r.msg.size() < hdr::kSize + kTrailerSize) return;
    if (read_header(r.msg.data()).type != MsgType::ExecutionReport) return;
    ExecReport er;
    if (!decode_exec_report(r.msg.data(), r.msg.size(), er)) return;
    if (er.exec_type == ExecType::Trade) {
      const std::string id(er.exec_id.view());
      if (by_exec.count(id)) return;  // replayed
      Fill f{id, std::string(er.cl_ord_id.view()), std::string(er.order_id.view()), std::string(er.security_id.view()),
             er.side, er.exec_qty, er.exec_price, r.ts};
      by_exec.emplace(id, f);
      order.push_back(id);
    } else if (er.exec_type == ExecType::TradeCancel) {
      by_exec.erase(std::string(er.ref_exec_id.view()));
    }
  });
  if (ok) *ok = read;
  std::vector<Fill> out;
  for (const auto& id : order)
    if (auto it = by_exec.find(id); it != by_exec.end()) out.push_back(it->second);
  return out;
}

// CSV: exec_id,cl_ord_id,order_id,security_id,side,qty,price (qty / price as fixed point x1e8)
inline void write_fills_csv(std::FILE* out, const std::vector<Fill>& fills) {
  std::fprintf(out, "exec_id,cl_ord_id,order_id,security_id,side,qty,price\n");
  for (const Fill& f : fills)
    std::fprintf(out, "%s,%s,%s,%s,%u,%lld,%lld\n", f.exec_id.c_str(), f.cl_ord_id.c_str(), f.order_id.c_str(),
                 f.security_id.c_str(), f.side, static_cast<long long>(f.qty), static_cast<long long>(f.price));
}

inline std::vector<Fill> read_fills_csv(const std::string& path) {
  std::vector<Fill> out;
  std::FILE* in = std::fopen(path.c_str(), "r");
  if (!in) return out;
  char line[512];
  bool header = true;
  while (std::fgets(line, sizeof line, in)) {
    if (header) {
      header = false;
      continue;
    }
    char e[64], c[64], o[64], s[64];
    unsigned side;
    long long q, p;
    if (std::sscanf(line, "%63[^,],%63[^,],%63[^,],%63[^,],%u,%lld,%lld", e, c, o, s, &side, &q, &p) == 7)
      out.push_back({e, c, o, s, static_cast<std::uint8_t>(side), q, p, 0});
  }
  std::fclose(in);
  return out;
}

struct ReconResult {
  std::vector<Fill> only_ours, only_theirs;
  std::vector<std::pair<Fill, Fill>> mismatched;  // same Execution ID, different content
  std::map<std::string, std::int64_t> our_position, their_position;  // net shares by security
  bool clean() const { return only_ours.empty() && only_theirs.empty() && mismatched.empty(); }
};

inline ReconResult reconcile(const std::vector<Fill>& ours, const std::vector<Fill>& theirs) {
  ReconResult r;
  std::map<std::string, const Fill*> t;
  for (const Fill& f : theirs) {
    t[f.exec_id] = &f;
    r.their_position[f.security_id] += f.side == 1 ? f.qty : -f.qty;
  }
  for (const Fill& f : ours) {
    r.our_position[f.security_id] += f.side == 1 ? f.qty : -f.qty;
    auto it = t.find(f.exec_id);
    if (it == t.end()) {
      r.only_ours.push_back(f);
      continue;
    }
    if (!(f == *it->second)) r.mismatched.emplace_back(f, *it->second);
    t.erase(it);
  }
  for (const auto& [id, f] : t) r.only_theirs.push_back(*f);
  return r;
}

}  // namespace obl::gw::ocgc
