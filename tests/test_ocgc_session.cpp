// OCG-C session state machine against a scripted peer.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "obl/gw/ocgc/session.hpp"

using namespace obl;
using namespace obl::gw::ocgc;

namespace {

constexpr std::uint64_t kSec = 1'000'000'000;

struct Frame {
  std::vector<std::uint8_t> bytes;
  Header h;
  SessionFields sf;
};

struct FakeTransport {
  std::vector<Frame> sent;
  bool closed = false;
  void send(const std::uint8_t* p, std::size_t n) {
    Frame f;
    f.bytes.assign(p, p + n);
    CHECK(valid_frame(p, n));
    f.h = read_header(p);
    if (is_admin(f.h.type)) CHECK(decode_session(p, n, f.h.type, f.sf));
    sent.push_back(std::move(f));
  }
  void close() { closed = true; }
  std::vector<Frame> take() { return std::exchange(sent, {}); }
};

struct FakeApp {
  int active = 0;
  std::vector<std::uint32_t> business_seqs;
  std::vector<std::uint32_t> not_sent;
  std::vector<CloseReason> closed;
  std::vector<ExecReport> reports;
  std::vector<std::string> report_ids;  // copies (string_views point into transient buffers)
  void on_session_active(const SessionFields&) { ++active; }
  void on_business(const Header& h, const std::uint8_t* m, std::size_t n) {
    business_seqs.push_back(h.seq);
    if (h.type == MsgType::ExecutionReport) {
      ExecReport er;
      CHECK(decode_exec_report(m, n, er));
      report_ids.emplace_back(er.exec_id);
      reports.push_back(er);
    }
  }
  void on_not_sent(std::uint32_t seq, const std::uint8_t*, std::size_t) { not_sent.push_back(seq); }
  void on_session_closed(CloseReason r) { closed.push_back(r); }
};

using TestSession = Session<FakeTransport, FakeApp>;

// The OCG-C side: builds messages with its own outbound sequence numbers.
struct Peer {
  std::uint32_t seq = 1;
  std::uint8_t buf[1024];
  std::vector<std::uint8_t> out;

  const std::vector<std::uint8_t>& keep(std::size_t n) {
    out.assign(buf, buf + n);
    return out;
  }
  const std::vector<std::uint8_t>& logon_reply(std::uint32_t next_expected) {
    return keep(encode_logon_reply(buf, seq++, "OCGC", next_expected));
  }
  const std::vector<std::uint8_t>& heartbeat() { return keep(encode_heartbeat(buf, seq++, "OCGC")); }
  const std::vector<std::uint8_t>& test_request(std::uint16_t id) { return keep(encode_test_request(buf, seq++, "OCGC", id)); }
  const std::vector<std::uint8_t>& resend_request(std::uint32_t a, std::uint32_t b) {
    return keep(encode_resend_request(buf, seq++, "OCGC", a, b));
  }
  const std::vector<std::uint8_t>& logout() { return keep(encode_logout(buf, seq++, "OCGC", "bye")); }
  const std::vector<std::uint8_t>& gap_fill(std::uint32_t at, std::uint32_t new_seq) {
    return keep(encode_sequence_reset(buf, at, "OCGC", new_seq, true, true));
  }
  // Execution report with an explicit sequence number (for gaps and replays).
  const std::vector<std::uint8_t>& report_at(std::uint32_t s, const std::string& exec_id, bool poss_dup = false) {
    ExecReport er;
    namespace b = exec_report;
    er.present = 1ull << b::ClOrdId | 1ull << b::SubmittingBrokerId | 1ull << b::SecurityId | 1ull << b::SecurityIdSource |
                 1ull << b::TransactTime | 1ull << b::Side | 1ull << b::OrderId | 1ull << b::ExecId |
                 1ull << b::OrdStatus | 1ull << b::ExecType | 1ull << b::CumQty | 1ull << b::LeavesQty;
    er.cl_ord_id = "10000001";
    er.submitting_broker_id = "1234";
    er.security_id = "700";
    er.transact_time = "20261003-01:30:00.000000";
    er.side = 1;
    er.order_id = "9000001";
    er.exec_id = std::string_view(exec_id);
    er.ord_status = OrdStatus::New;
    er.exec_type = ExecType::New;
    er.leaves_qty = 100 * kDecimalScale;
    return keep(encode_exec_report(buf, s, "OCGC", er, poss_dup));
  }
  const std::vector<std::uint8_t>& report(const std::string& exec_id) { return report_at(seq++, exec_id); }
};

SessionConfig config(ReplayPolicy policy = ReplayPolicy::Replay) {
  SessionConfig c;
  c.comp_id = "CO99999901";
  c.encrypted_password = "ENCRYPTED";
  c.replay = policy;
  c.store_reserve_bytes = 1 << 16;
  c.store_reserve_msgs = 1 << 10;
  return c;
}

const NewOrderStatic kStatic{"CO99999901", "1234", "700", "ABC123.2568", WireSide::Buy, 20261003};

NewOrderVar order(std::uint32_t id) { return {0, id, 3600ull * 1'000'000, 500 * kDecimalScale, 100 * kDecimalScale}; }

void feed(TestSession& s, const std::vector<std::uint8_t>& b, std::uint64_t now) { s.on_bytes(b.data(), b.size(), now); }

// Connect and complete the logon exchange.
void logon(TestSession& s, FakeTransport& tx, Peer& peer, std::uint64_t now) {
  s.on_connected(now);
  auto f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::Logon);
  feed(s, peer.logon_reply(s.next_out_seq()), now);
  CHECK(s.active());
}

void test_logon_and_send() {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(), tx, app);
  NewOrderTemplate tpl(kStatic);

  CHECK(!s.send_new_order(tpl, order(10000001), 0));  // not connected: rejected locally
  s.on_connected(0);
  auto f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::Logon);
  CHECK_EQ(f[0].h.seq, 1u);
  CHECK_EQ(f[0].sf.next_expected, 1u);
  CHECK_EQ(f[0].bytes.size(), 54u + 450 + 4 + 4);
  CHECK(s.state() == SessionState::LogonSent);
  CHECK(!s.send_new_order(tpl, order(10000001), 0));  // must wait for the logon reply (§4.1)

  feed(s, peer.logon_reply(2), 1);
  CHECK(s.active());
  CHECK_EQ(app.active, 1);
  CHECK_EQ(s.next_in_seq(), 2u);
  CHECK(tx.take().empty());

  CHECK(s.send_new_order(tpl, order(10000001), 2));
  f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::NewOrder);
  CHECK_EQ(f[0].h.seq, 2u);
  CHECK_EQ(s.store().last_seq(), 2u);

  feed(s, peer.report("E1"), 3);
  CHECK_EQ(app.reports.size(), 1u);
  CHECK(app.report_ids[0] == "E1");
  CHECK(app.reports[0].exec_type == ExecType::New);
  CHECK_EQ(app.reports[0].leaves_qty, 100 * kDecimalScale);
  CHECK_EQ(s.next_in_seq(), 3u);
}

void test_heartbeat_and_test_request() {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(), tx, app);
  logon(s, tx, peer, 0);

  s.on_timer(19 * kSec);
  CHECK(tx.take().empty());
  s.on_timer(20 * kSec);  // nothing sent for 20 s -> Heartbeat
  auto f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::Heartbeat);

  feed(s, peer.test_request(77), 21 * kSec);  // peer asks: we answer with its ID
  f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::Heartbeat);
  CHECK_EQ(f[0].sf.test_req_id, 77);

  // peer silent for 3 intervals -> Test Request
  s.on_timer(21 * kSec + 60 * kSec);
  f = tx.take();
  CHECK(!f.empty());
  CHECK(f[0].h.type == MsgType::TestRequest);
  // answered in time
  feed(s, peer.heartbeat(), 21 * kSec + 61 * kSec);
  s.on_timer(21 * kSec + 100 * kSec);
  CHECK(app.closed.empty());

  // silent again, Test Request, no answer for 3 intervals -> Logout and close
  s.on_timer(21 * kSec + 61 * kSec + 60 * kSec);
  tx.take();
  s.on_timer(21 * kSec + 61 * kSec + 120 * kSec);
  f = tx.take();
  CHECK(!f.empty() && f.back().h.type == MsgType::Logout);
  CHECK(tx.closed);
  CHECK_EQ(app.closed.size(), 1u);
  CHECK(app.closed[0] == CloseReason::HeartbeatTimeout);
}

void test_inbound_gap() {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(), tx, app);
  logon(s, tx, peer, 0);  // peer logon = seq 1

  feed(s, peer.report_at(2, "E2"), 1);
  feed(s, peer.report_at(5, "E5"), 2);  // 3, 4 lost
  auto f = tx.take();
  CHECK_EQ(f.size(), 1u);
  CHECK(f[0].h.type == MsgType::ResendRequest);
  CHECK_EQ(f[0].sf.start_seq, 3u);
  CHECK_EQ(f[0].sf.end_seq, 0u);
  CHECK(s.recovering());
  feed(s, peer.report_at(6, "E6"), 3);  // still in recovery: dropped, no second request
  CHECK(tx.take().empty());
  CHECK_EQ(app.reports.size(), 1u);

  // replay 3..6 (PossDup), then live 7
  for (std::uint32_t q = 3; q <= 6; ++q) feed(s, peer.report_at(q, "E" + std::to_string(q), true), 4);
  CHECK(!s.recovering());
  feed(s, peer.report_at(7, "E7"), 5);
  CHECK_EQ(app.report_ids.size(), 6u);
  const char* want[] = {"E2", "E3", "E4", "E5", "E6", "E7"};
  for (std::size_t i = 0; i < 6; ++i) CHECK(app.report_ids[i] == want[i]);
  CHECK_EQ(s.next_in_seq(), 8u);

  // duplicate with PossDup is ignored; without PossDup it is fatal
  feed(s, peer.report_at(4, "dup", true), 6);
  CHECK_EQ(app.reports.size(), 6u);
  CHECK(app.closed.empty());
  feed(s, peer.report_at(4, "bad"), 7);
  CHECK_EQ(app.closed.size(), 1u);
  CHECK(app.closed[0] == CloseReason::SeqTooLow);
  CHECK(tx.take().back().h.type == MsgType::Logout);
}

// Peer asks us to resend 2..: Logon(1), NewOrder(2), NewOrder(3), Heartbeat(4), NewOrder(5)
void test_resend_request(ReplayPolicy policy) {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(policy), tx, app);
  NewOrderTemplate tpl(kStatic);
  logon(s, tx, peer, 0);
  CHECK(s.send_new_order(tpl, order(10000001), 1));
  CHECK(s.send_new_order(tpl, order(10000002), 1));
  s.on_timer(1 + 20 * kSec);  // heartbeat = 4
  CHECK(s.send_new_order(tpl, order(10000003), 25 * kSec));
  auto original = tx.take();
  CHECK_EQ(original.size(), 4u);
  CHECK(original[2].h.type == MsgType::Heartbeat);

  feed(s, peer.resend_request(2, 0), 26 * kSec);
  auto f = tx.take();
  if (policy == ReplayPolicy::Replay) {
    CHECK_EQ(f.size(), 4u);
    // 2, 3 replayed: same bytes except PossDup and checksum
    for (int i = 0; i < 2; ++i) {
      CHECK(f[i].h.type == MsgType::NewOrder);
      CHECK_EQ(f[i].h.seq, 2u + i);
      CHECK_EQ(f[i].h.poss_dup, 1);
      CHECK_EQ(f[i].bytes.size(), original[i].bytes.size());
      CHECK(std::memcmp(f[i].bytes.data() + hdr::kPossResend, original[i].bytes.data() + hdr::kPossResend,
                        f[i].bytes.size() - hdr::kPossResend - kTrailerSize) == 0);
    }
    CHECK(f[2].h.type == MsgType::SequenceReset);  // heartbeat skipped
    CHECK_EQ(f[2].h.seq, 4u);
    CHECK_EQ(f[2].sf.new_seq, 5u);
    CHECK_EQ(f[2].sf.gap_fill, 'Y');
    CHECK(f[3].h.type == MsgType::NewOrder);
    CHECK_EQ(f[3].h.seq, 5u);
    CHECK(app.not_sent.empty());
  } else {
    CHECK_EQ(f.size(), 1u);  // one gap fill over 2..5
    CHECK(f[0].h.type == MsgType::SequenceReset);
    CHECK_EQ(f[0].h.seq, 2u);
    CHECK_EQ(f[0].sf.new_seq, 6u);
    CHECK_EQ(app.not_sent.size(), 3u);
    CHECK_EQ(app.not_sent[0], 2u);
    CHECK_EQ(app.not_sent[2], 5u);
  }
  CHECK_EQ(s.next_out_seq(), 6u);  // replays reuse old numbers
}

void test_reconnect_outbound_lost() {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(), tx, app);
  NewOrderTemplate tpl(kStatic);
  logon(s, tx, peer, 0);                            // our 1, peer 1
  CHECK(s.send_new_order(tpl, order(10000001), 1));  // 2 (received)
  CHECK(s.send_new_order(tpl, order(10000002), 1));  // 3 (lost in the disconnect)
  tx.take();
  s.on_disconnected();
  CHECK(!s.send_new_order(tpl, order(10000003), 2));

  s.on_connected(10 * kSec);  // Logon = our 4
  auto f = tx.take();
  CHECK_EQ(f[0].h.seq, 4u);
  CHECK_EQ(f[0].sf.next_expected, 2u);
  feed(s, peer.logon_reply(3), 10 * kSec);  // peer got up to 2
  f = tx.take();
  CHECK_EQ(f.size(), 2u);
  CHECK(f[0].h.type == MsgType::NewOrder && f[0].h.seq == 3u && f[0].h.poss_dup == 1);
  CHECK(f[1].h.type == MsgType::SequenceReset && f[1].h.seq == 4u && f[1].sf.new_seq == 5u);
  CHECK(s.active());
  CHECK(s.send_new_order(tpl, order(10000003), 11 * kSec));
  CHECK_EQ(tx.take()[0].h.seq, 5u);

  // peer expects more than we ever sent: manual intervention (§5.3 case 1)
  s.on_disconnected();
  s.on_connected(20 * kSec);
  tx.take();
  feed(s, peer.logon_reply(99), 20 * kSec);
  CHECK(app.closed.back() == CloseReason::NextExpectedTooHigh);
}

void test_reconnect_inbound_lost() {
  FakeTransport tx;
  FakeApp app;
  Peer peer;
  TestSession s(config(), tx, app);
  logon(s, tx, peer, 0);                // peer 1
  feed(s, peer.report("E2"), 1);        // peer 2
  peer.report("E3");                    // 3, 4 sent by peer but lost in the disconnect
  peer.report("E4");
  s.on_disconnected();

  s.on_connected(10 * kSec);
  auto f = tx.take();
  CHECK_EQ(f[0].sf.next_expected, 3u);
  feed(s, peer.logon_reply(s.next_out_seq()), 10 * kSec);  // peer Logon = 5
  CHECK(s.active());
  CHECK(tx.take().empty());  // no Resend Request off a Logon (§5.3)
  CHECK(s.recovering());
  feed(s, peer.report_at(3, "E3", true), 10 * kSec);
  feed(s, peer.report_at(4, "E4", true), 10 * kSec);
  feed(s, peer.gap_fill(5, 6), 10 * kSec);  // skips the peer's Logon
  CHECK(!s.recovering());
  CHECK_EQ(s.next_in_seq(), 6u);
  feed(s, peer.report("E6"), 11 * kSec);
  CHECK_EQ(app.report_ids.size(), 4u);
  CHECK(app.report_ids[3] == "E6");
}

void test_logout() {
  {
    FakeTransport tx;
    FakeApp app;
    Peer peer;
    TestSession s(config(), tx, app);
    logon(s, tx, peer, 0);
    s.logout(1, "end of day");
    CHECK(s.state() == SessionState::LogoutSent);
    CHECK(tx.take().back().h.type == MsgType::Logout);
    feed(s, peer.logout(), 2);
    CHECK(app.closed.back() == CloseReason::LogoutComplete);
    CHECK(tx.take().empty());  // no second Logout
  }
  {
    FakeTransport tx;
    FakeApp app;
    Peer peer;
    TestSession s(config(), tx, app);
    logon(s, tx, peer, 0);
    feed(s, peer.logout(), 1);
    CHECK(tx.take().back().h.type == MsgType::Logout);  // reply
    CHECK(app.closed.back() == CloseReason::PeerLogout);
  }
  {
    FakeTransport tx;
    FakeApp app;
    Peer peer;
    TestSession s(config(), tx, app);
    s.on_connected(0);
    tx.take();
    feed(s, peer.logout(), 1);  // logon refused
    CHECK(app.closed.back() == CloseReason::LogonRejected);
    s.on_connected(2);
    s.on_timer(2 + 61 * kSec);
    CHECK(app.closed.back() == CloseReason::LogonTimeout);
  }
}

void test_bad_checksum_and_fragments() {
  {
    FakeTransport tx;
    FakeApp app;
    Peer peer;
    TestSession s(config(), tx, app);
    logon(s, tx, peer, 0);
    auto m = peer.report("E2");
    m[60] ^= 0x40;
    feed(s, m, 1);
    CHECK(app.closed.back() == CloseReason::BadChecksum);
    CHECK(tx.take().empty());  // §4.8: no Logout
    CHECK(app.reports.empty());
  }
  {
    // several messages delivered one byte at a time, then all at once in one buffer
    FakeTransport tx;
    FakeApp app;
    Peer peer;
    TestSession s(config(), tx, app);
    logon(s, tx, peer, 0);
    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
      auto m = peer.report("F" + std::to_string(i));
      stream.insert(stream.end(), m.begin(), m.end());
    }
    for (auto byte : stream) s.on_bytes(&byte, 1, 1);
    CHECK_EQ(app.reports.size(), 3u);
    stream.clear();
    for (int i = 3; i < 6; ++i) {
      auto m = peer.report("F" + std::to_string(i));
      stream.insert(stream.end(), m.begin(), m.end());
    }
    s.on_bytes(stream.data(), stream.size() - 10, 2);  // last one split
    CHECK_EQ(app.reports.size(), 5u);
    s.on_bytes(stream.data() + stream.size() - 10, 10, 2);
    CHECK_EQ(app.reports.size(), 6u);
    CHECK(app.report_ids[5] == "F5");
    CHECK(app.closed.empty());
  }
}

}  // namespace

int main() {
  test_logon_and_send();
  test_heartbeat_and_test_request();
  test_inbound_gap();
  test_resend_request(ReplayPolicy::Replay);
  test_resend_request(ReplayPolicy::GapFillBusiness);
  test_reconnect_outbound_lost();
  test_reconnect_inbound_lost();
  test_logout();
  test_bad_checksum_and_fragments();
  return test_result("test_ocgc_session");
}
