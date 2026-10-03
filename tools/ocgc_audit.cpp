// OCG-C audit log tool.
//
//   obl_ocgc_audit dump <audit>                one line per message
//   obl_ocgc_audit fills <audit>               our fills as CSV (deduplicated by Execution ID)
//   obl_ocgc_audit reconcile <audit> <csv>     our fills against another record of the day's trades

#include <cstdio>
#include <cstring>
#include <string>

#include "obl/gw/audit.hpp"
#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/reconcile.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

const char* type_name(MsgType t) {
  switch (t) {
    case MsgType::Heartbeat: return "Heartbeat";
    case MsgType::TestRequest: return "TestRequest";
    case MsgType::ResendRequest: return "ResendRequest";
    case MsgType::Reject: return "Reject";
    case MsgType::SequenceReset: return "SequenceReset";
    case MsgType::Logon: return "Logon";
    case MsgType::Logout: return "Logout";
    case MsgType::LookupRequest: return "LookupRequest";
    case MsgType::LookupResponse: return "LookupResponse";
    case MsgType::BusinessMessageReject: return "BusinessReject";
    case MsgType::ExecutionReport: return "ExecReport";
    case MsgType::NewOrder: return "NewOrder";
    case MsgType::AmendOrder: return "Amend";
    case MsgType::CancelOrder: return "Cancel";
    case MsgType::MassCancel: return "MassCancel";
    case MsgType::OrderMassCancelReport: return "MassCancelReport";
  }
  return "?";
}

std::string time_of(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000 % 86400;
  char b[32];
  std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % 1'000'000'000));
  return b;
}

double px(std::int64_t v) { return static_cast<double>(v) / 1e8; }

void dump_one(const AuditRecord& r) {
  const Header h = read_header(r.msg.data());
  std::printf("%s %s r%u %-16s seq=%-6u%s%s", time_of(r.ts).c_str(), r.dir == AuditDir::In ? "<-" : "->", r.route,
              type_name(h.type), h.seq, h.poss_dup ? " PossDup" : "", h.poss_resend ? " PossResend" : "");
  const std::uint8_t* m = r.msg.data();
  const std::size_t n = r.msg.size();
  switch (h.type) {
    case MsgType::NewOrder:
    case MsgType::AmendOrder:
    case MsgType::CancelOrder:
    case MsgType::MassCancel: {
      OrderRequest q;
      if (decode_request(m, n, h.type, q))
        std::printf(" id=%u orig=%u sec=%s side=%u qty=%.0f px=%.3f", q.cl_ord_id, q.orig_cl_ord_id,
                    std::string(q.security_id.view()).c_str(), q.side, px(q.qty), px(q.price));
      break;
    }
    case MsgType::ExecutionReport: {
      ExecReport e;
      if (decode_exec_report(m, n, e))
        std::printf(" id=%s exec=%s type=%c status=%u cum=%.0f leaves=%.0f last=%.0f@%.3f",
                    std::string(e.cl_ord_id.view()).c_str(), std::string(e.exec_id.view()).c_str(),
                    static_cast<char>(e.exec_type), static_cast<unsigned>(e.ord_status), px(e.cum_qty),
                    px(e.leaves_qty), px(e.exec_qty), px(e.exec_price));
      break;
    }
    case MsgType::SequenceReset:
    case MsgType::ResendRequest:
    case MsgType::Logon: {
      SessionFields f;
      if (decode_session(m, n, h.type, f))
        std::printf(" next_expected=%u start=%u end=%u new_seq=%u", f.next_expected, f.start_seq, f.end_seq, f.new_seq);
      break;
    }
    case MsgType::BusinessMessageReject:
    case MsgType::Reject: {
      RejectInfo ri;
      if (decode_reject(m, n, h.type, ri))
        std::printf(" code=%u ref=%s reason=\"%s\"", ri.code, std::string(ri.ref_id).c_str(), std::string(ri.reason).c_str());
      break;
    }
    default:
      break;
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s dump|fills <audit> | reconcile <audit> <fills.csv>\n", argv[0]);
    return 2;
  }
  const std::string cmd = argv[1], path = argv[2];
  if (cmd == "dump") {
    if (!read_audit(path, dump_one)) {
      std::fprintf(stderr, "%s: not an audit log\n", path.c_str());
      return 1;
    }
    return 0;
  }
  bool ok = false;
  const auto ours = fills_from_audit(path, &ok);
  if (!ok) {
    std::fprintf(stderr, "%s: not an audit log\n", path.c_str());
    return 1;
  }
  if (cmd == "fills") {
    write_fills_csv(stdout, ours);
    return 0;
  }
  if (cmd == "reconcile" && argc >= 4) {
    const auto r = reconcile(ours, read_fills_csv(argv[3]));
    for (const auto& f : r.only_ours) std::printf("only in audit:  %s %s %lld@%lld\n", f.exec_id.c_str(), f.security_id.c_str(), static_cast<long long>(f.qty), static_cast<long long>(f.price));
    for (const auto& f : r.only_theirs) std::printf("only in file:   %s %s %lld@%lld\n", f.exec_id.c_str(), f.security_id.c_str(), static_cast<long long>(f.qty), static_cast<long long>(f.price));
    for (const auto& [a, b] : r.mismatched) std::printf("mismatch:       %s qty %lld/%lld px %lld/%lld\n", a.exec_id.c_str(), static_cast<long long>(a.qty), static_cast<long long>(b.qty), static_cast<long long>(a.price), static_cast<long long>(b.price));
    for (const auto& [sec, q] : r.our_position) {
      const auto it = r.their_position.find(sec);
      const long long t = it == r.their_position.end() ? 0 : it->second;
      std::printf("position %-6s ours %.0f theirs %.0f%s\n", sec.c_str(), px(q), static_cast<double>(t) / 1e8, q == t ? "" : "  <-- differs");
    }
    std::printf("%s: %zu fills, %zu only ours, %zu only theirs, %zu mismatched\n", r.clean() ? "CLEAN" : "BREAKS",
                ours.size(), r.only_ours.size(), r.only_theirs.size(), r.mismatched.size());
    return r.clean() ? 0 : 1;
  }
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
