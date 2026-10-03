#pragma once

// OCG-C binary session (client side), §4 and §5.
//
// Transport-agnostic: the owner feeds received bytes (on_bytes), connection events
// (on_connected / on_disconnected) and the clock (on_timer, and `now` on every call); the session
// writes through a Transport and reports to a Handler. Nothing here blocks, allocates on the send
// path, or reads a clock itself, so the same code runs over a TCP socket, kernel bypass, or a test.
//
//   Transport: void send(const std::uint8_t*, std::size_t);  void close();
//   Handler:   void on_session_active(const SessionFields& logon_reply);
//              void on_business(const Header&, const std::uint8_t* msg, std::size_t len);
//              void on_not_sent(std::uint32_t seq, const std::uint8_t* msg, std::size_t len);
//              void on_session_closed(CloseReason);
//
// Sequence numbers (§4.2): both directions start at 1 each trading day and carry on across
// reconnects within the day; they live in this object, reset_sequences() starts a new day.
//
// Recovery (§5):
//  - inbound gap (seq > expected): one Resend Request(expected, 0), out-of-order messages are
//    dropped (the peer replays everything from `expected`), no second request until caught up
//  - seq < expected: ignored if PossDup, otherwise Logout and disconnect
//  - Logon reply: never triggers a Resend Request (§5.3); the peer replays from our Next
//    Expected and gap-fills its Logon's own sequence number. If its Next Expected is below ours,
//    we replay to it the same way.
//  - Resend Request from the peer: session-level messages are skipped with Sequence Reset
//    (gap fill); business messages are replayed with PossDup = 1 (ReplayPolicy::Replay, what
//    §5.6 expects) or also gap-filled and handed back to the application as not sent
//    (ReplayPolicy::GapFillBusiness: a New Order that never reached the exchange is not sent
//    late, the strategy decides again).

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "obl/gw/ocgc/messages.hpp"
#include "obl/gw/ocgc/order_template.hpp"
#include "obl/gw/ocgc/protocol.hpp"

namespace obl::gw::ocgc {

enum class SessionState : std::uint8_t { Disconnected, LogonSent, Active, LogoutSent };

enum class ReplayPolicy : std::uint8_t { Replay, GapFillBusiness };

enum class CloseReason : std::uint8_t {
  LogonRejected,     // Logout / Reject / bad Session Status in reply to our Logon
  LogonTimeout,      // no Logon reply within logon_timeout (§4.1: 60 s)
  LogoutComplete,    // we sent Logout, peer replied
  PeerLogout,        // peer sent Logout, we replied
  LogoutTimeout,     // we sent Logout, no reply within logout_timeout (§4.4: 60 s)
  BadFrame,          // no STX / impossible length
  BadChecksum,       // §4.8: drop the connection without Logout
  SeqTooLow,         // seq < expected without PossDup (§4.2)
  NextExpectedTooHigh,  // peer expects a sequence number we have not sent (§5.3 case 1)
  HeartbeatTimeout,  // no answer to Test Request (§4.3)
  ProtocolError,
};

inline const char* to_string(CloseReason r) {
  switch (r) {
    case CloseReason::LogonRejected: return "logon rejected";
    case CloseReason::LogonTimeout: return "logon timeout";
    case CloseReason::LogoutComplete: return "logout complete";
    case CloseReason::PeerLogout: return "peer logout";
    case CloseReason::LogoutTimeout: return "logout timeout";
    case CloseReason::BadFrame: return "bad frame";
    case CloseReason::BadChecksum: return "bad checksum";
    case CloseReason::SeqTooLow: return "sequence number too low";
    case CloseReason::NextExpectedTooHigh: return "next expected too high";
    case CloseReason::HeartbeatTimeout: return "heartbeat timeout";
    case CloseReason::ProtocolError: return "protocol error";
  }
  return "?";
}

struct SessionConfig {
  std::string comp_id;
  std::string encrypted_password;  // already RSA-encrypted as OCG-C requires (§3.5)
  std::uint64_t heartbeat_ns = 20'000'000'000;  // §4.3: 20 s
  std::uint32_t test_request_after = 3;         // heartbeat intervals of silence before Test Request
  std::uint32_t test_request_timeout = 3;       // heartbeat intervals to wait for the answer
  std::uint64_t logon_timeout_ns = 60'000'000'000;
  std::uint64_t logout_timeout_ns = 60'000'000'000;
  ReplayPolicy replay = ReplayPolicy::Replay;
  std::size_t store_reserve_bytes = 64u << 20;
  std::size_t store_reserve_msgs = 1u << 20;
  bool prefault_store = true;
};

// Every message we sent today, by sequence number, for replay.
class MessageStore {
 public:
  // prefault: touch every page now, so appends on the send path never take a page fault
  void reserve(std::size_t bytes, std::size_t msgs, bool prefault) {
    if (prefault) {
      bytes_.resize(bytes);
      entries_.resize(msgs);
      bytes_.clear();
      entries_.clear();
    }
    bytes_.reserve(bytes);
    entries_.reserve(msgs);
  }
  void append(std::uint32_t seq, const std::uint8_t* msg, std::size_t len) {
    assert(seq == entries_.size() + 1);
    (void)seq;
    entries_.push_back({bytes_.size(), static_cast<std::uint32_t>(len)});
    bytes_.insert(bytes_.end(), msg, msg + len);
  }
  std::uint32_t last_seq() const { return static_cast<std::uint32_t>(entries_.size()); }
  const std::uint8_t* data(std::uint32_t seq) const { return bytes_.data() + entries_[seq - 1].off; }
  std::size_t size(std::uint32_t seq) const { return entries_[seq - 1].len; }
  MsgType type(std::uint32_t seq) const { return static_cast<MsgType>(data(seq)[hdr::kMsgType]); }
  void clear() {
    bytes_.clear();
    entries_.clear();
  }

 private:
  struct Entry {
    std::size_t off;
    std::uint32_t len;
  };
  std::vector<std::uint8_t> bytes_;
  std::vector<Entry> entries_;
};

template <class Transport, class Handler>
class Session {
 public:
  static constexpr std::size_t kMaxMessage = 4096;
  static constexpr std::size_t kRxCapacity = 4 * kMaxMessage;

  Session(SessionConfig cfg, Transport& tx, Handler& app) : cfg_(std::move(cfg)), tx_(tx), app_(app) {
    store_.reserve(cfg_.store_reserve_bytes, cfg_.store_reserve_msgs, cfg_.prefault_store);
  }

  SessionState state() const { return state_; }
  bool active() const { return state_ == SessionState::Active; }
  std::uint32_t next_out_seq() const { return next_out_; }
  std::uint32_t next_in_seq() const { return next_in_; }
  bool recovering() const { return resend_outstanding_; }
  const MessageStore& store() const { return store_; }

  // Start of a trading day (or after HKEX asks for a manual reset, §4.6.2.2).
  void reset_sequences(std::uint32_t next_out = 1, std::uint32_t next_in = 1) {
    assert(state_ == SessionState::Disconnected);
    store_.clear();
    next_out_ = next_out;
    next_in_ = next_in;
    // keep store indices aligned with sequence numbers if starting above 1
    for (std::uint32_t s = 1; s < next_out; ++s) {
      const std::size_t n = encode_heartbeat(scratch_, s, cfg_.comp_id);
      store_.append(s, scratch_, n);
    }
  }

  // ------------------------------------------------------------------------------------------
  // Connection events

  void on_connected(std::uint64_t now) {
    rx_len_ = 0;
    resend_outstanding_ = false;
    gap_max_ = 0;
    test_req_pending_ = false;
    last_recv_ = now;
    set_state(SessionState::LogonSent, now);
    const std::size_t n = encode_logon(scratch_, next_out_, cfg_.comp_id, cfg_.encrypted_password, next_in_);
    send_new(scratch_, n, now);
  }

  void on_disconnected() { state_ = SessionState::Disconnected; }

  void logout(std::uint64_t now, std::string_view text = {}) {
    if (state_ != SessionState::Active) return;
    send_new(scratch_, encode_logout(scratch_, next_out_, cfg_.comp_id, text), now);
    set_state(SessionState::LogoutSent, now);
  }

  // ------------------------------------------------------------------------------------------
  // Hot path: send a New Order from a pre-encoded template. Returns false (nothing sent) unless
  // the session is active; the caller rejects the order back to the strategy.
  bool send_new_order(NewOrderTemplate& t, NewOrderVar v, std::uint64_t now) {
    if (state_ != SessionState::Active) [[unlikely]]
      return false;
    v.seq = next_out_;
    const std::uint8_t* m = t.fill_regs(v);
    tx_.send(m, t.size());
    // bookkeeping only after the bytes are handed to the transport
    store_.append(next_out_++, m, t.size());
    last_send_ = now;
    return true;
  }

  // Any fully built business message (Cancel, Amend, ...): stamps the sequence number and
  // recomputes the checksum.
  bool send_business(std::uint8_t* msg, std::size_t len, std::uint64_t now) {
    if (state_ != SessionState::Active) [[unlikely]]
      return false;
    stamp(msg, len, next_out_, false);
    send_new(msg, len, now);
    return true;
  }

  // ------------------------------------------------------------------------------------------
  // Receive path

  void on_bytes(const std::uint8_t* data, std::size_t n, std::uint64_t now) {
    if (state_ == SessionState::Disconnected) return;
    if (rx_len_ == 0) {
      // common case: whole messages, parsed straight from the caller's buffer
      const std::size_t used = consume(data, n, now);
      if (state_ == SessionState::Disconnected) return;
      data += used;
      n -= used;
    }
    // partial message: accumulate in the fixed receive buffer (never holds more than one
    // partial message after consume, so there is always room)
    while (n > 0) {
      const std::size_t take = std::min(n, kRxCapacity - rx_len_);
      std::memcpy(rx_ + rx_len_, data, take);
      rx_len_ += take;
      data += take;
      n -= take;
      const std::size_t used = consume(rx_, rx_len_, now);
      if (state_ == SessionState::Disconnected) {
        rx_len_ = 0;
        return;
      }
      std::memmove(rx_, rx_ + used, rx_len_ - used);
      rx_len_ -= used;
    }
  }

  void on_timer(std::uint64_t now) {
    switch (state_) {
      case SessionState::LogonSent:
        if (now - state_since_ >= cfg_.logon_timeout_ns) close(CloseReason::LogonTimeout);
        return;
      case SessionState::LogoutSent:
        if (now - state_since_ >= cfg_.logout_timeout_ns) close(CloseReason::LogoutTimeout);
        return;
      case SessionState::Active: {
        const std::uint64_t hb = cfg_.heartbeat_ns;
        if (test_req_pending_) {
          if (now - test_req_sent_ >= hb * cfg_.test_request_timeout) {
            fatal_logout(CloseReason::HeartbeatTimeout, "no response to test request", now);
            return;
          }
        } else if (now - last_recv_ >= hb * cfg_.test_request_after) {
          send_new(scratch_, encode_test_request(scratch_, next_out_, cfg_.comp_id, ++test_req_id_), now);
          test_req_pending_ = true;
          test_req_sent_ = now;
        }
        if (now - last_send_ >= hb) send_new(scratch_, encode_heartbeat(scratch_, next_out_, cfg_.comp_id), now);
        return;
      }
      case SessionState::Disconnected:
        return;
    }
  }

 private:
  // Returns bytes consumed (whole messages only).
  std::size_t consume(const std::uint8_t* p, std::size_t n, std::uint64_t now) {
    std::size_t off = 0;
    while (n - off >= 3) {
      const std::uint8_t* m = p + off;
      if (m[hdr::kStart] != kStx) return fail(CloseReason::BadFrame, n);
      const std::size_t len = load_le<std::uint16_t>(m + hdr::kLength);
      if (len < hdr::kSize + kTrailerSize || len > kMaxMessage) return fail(CloseReason::BadFrame, n);
      if (n - off < len) break;
      if (!verify_checksum(m, len)) return fail(CloseReason::BadChecksum, n);
      process(m, len, now);
      if (state_ == SessionState::Disconnected) return n;
      off += len;
    }
    return off;
  }

  void process(const std::uint8_t* msg, std::size_t len, std::uint64_t now) {
    const Header h = read_header(msg);
    last_recv_ = now;
    test_req_pending_ = false;

    if (state_ == SessionState::LogonSent) {
      if (h.type == MsgType::Logon) return on_logon_reply(h, msg, len, now);
      return close(CloseReason::LogonRejected);  // Logout, Reject, or anything else
    }

    SessionFields sf;
    const bool is_session = is_admin(h.type);
    if (is_session && !decode_session(msg, len, h.type, sf)) return fatal_logout(CloseReason::ProtocolError, "malformed", now);

    // Sequence Reset in reset mode ignores the sequence number (§4.6, OCG-C only)
    if (h.type == MsgType::SequenceReset && sf.gap_fill != sequence_reset::kGapFill) {
      if (sf.new_seq < next_in_) return fatal_logout(CloseReason::ProtocolError, "reset to lower sequence", now);
      next_in_ = sf.new_seq;
      update_recovery();
      return;
    }

    if (h.seq > next_in_) {
      gap_max_ = std::max(gap_max_, h.seq);
      if (!resend_outstanding_) {
        send_new(scratch_, encode_resend_request(scratch_, next_out_, cfg_.comp_id, next_in_, 0), now);
        resend_outstanding_ = true;
      }
      // serve the peer's resend request even across our gap, so neither side waits on the other
      if (h.type == MsgType::ResendRequest) serve_resend(sf.start_seq, sf.end_seq, now);
      return;  // dropped: will come again in the replay
    }
    if (h.seq < next_in_) {
      if (h.poss_dup) return;
      return fatal_logout(CloseReason::SeqTooLow, "sequence number too low", now);
    }

    // h.seq == next_in_
    if (h.type == MsgType::SequenceReset) {  // gap fill
      if (sf.new_seq <= h.seq) return fatal_logout(CloseReason::ProtocolError, "bad gap fill", now);
      next_in_ = sf.new_seq;
      update_recovery();
      return;
    }
    ++next_in_;
    update_recovery();

    switch (h.type) {
      case MsgType::Heartbeat:
        return;
      case MsgType::TestRequest:
        send_new(scratch_, encode_heartbeat(scratch_, next_out_, cfg_.comp_id, sf.test_req_id), now);
        return;
      case MsgType::ResendRequest:
        return serve_resend(sf.start_seq, sf.end_seq, now);
      case MsgType::Logout:
        if (state_ != SessionState::LogoutSent)
          send_new(scratch_, encode_logout(scratch_, next_out_, cfg_.comp_id), now);
        return close(state_ == SessionState::LogoutSent ? CloseReason::LogoutComplete : CloseReason::PeerLogout);
      case MsgType::Logon:
        return fatal_logout(CloseReason::ProtocolError, "unexpected logon", now);
      default:
        app_.on_business(h, msg, len);
        return;
    }
  }

  void on_logon_reply(const Header& h, const std::uint8_t* msg, std::size_t len, std::uint64_t now) {
    SessionFields sf;
    if (!decode_session(msg, len, MsgType::Logon, sf)) return close(CloseReason::ProtocolError);
    if (sf.session_status != static_cast<int>(SessionStatus::Active) &&
        sf.session_status != static_cast<int>(SessionStatus::PasswordChanged))
      return close(CloseReason::LogonRejected);
    if (sf.next_expected > next_out_) return fatal_logout(CloseReason::NextExpectedTooHigh, "next expected too high", now);
    if (h.seq < next_in_) return fatal_logout(CloseReason::SeqTooLow, "logon sequence too low", now);
    if (h.seq == next_in_) {
      ++next_in_;
    } else {
      // The peer replays [next_in_, h.seq) and gap-fills h.seq; no Resend Request (§5.3).
      resend_outstanding_ = true;
      gap_max_ = h.seq;
    }
    set_state(SessionState::Active, now);
    if (sf.next_expected < next_out_) serve_resend(sf.next_expected, next_out_ - 1, now);
    app_.on_session_active(sf);
  }

  void serve_resend(std::uint32_t start, std::uint32_t end, std::uint64_t now) {
    const std::uint32_t last = next_out_ - 1;
    if (end == 0 || end > last) end = last;
    if (start == 0 || start > end) return;
    std::uint32_t seq = start;
    while (seq <= end) {
      if (skip_on_replay(seq)) {
        std::uint32_t run_end = seq;
        while (run_end < end && skip_on_replay(run_end + 1)) ++run_end;
        for (std::uint32_t s = seq; s <= run_end; ++s)
          if (!is_admin(store_.type(s))) app_.on_not_sent(s, store_.data(s), store_.size(s));
        const std::size_t n = encode_sequence_reset(scratch_, seq, cfg_.comp_id, run_end + 1, true, true);
        tx_.send(scratch_, n);  // reuses an old sequence number: not stored again
        seq = run_end + 1;
      } else {
        const std::size_t n = store_.size(seq);
        std::memcpy(replay_, store_.data(seq), n);
        stamp(replay_, n, seq, true);
        tx_.send(replay_, n);
        ++seq;
      }
    }
    last_send_ = now;
  }

  bool skip_on_replay(std::uint32_t seq) const {
    return is_admin(store_.type(seq)) || cfg_.replay == ReplayPolicy::GapFillBusiness;
  }

  void update_recovery() {
    if (resend_outstanding_ && next_in_ > gap_max_) resend_outstanding_ = false;
  }

  static void stamp(std::uint8_t* msg, std::size_t len, std::uint32_t seq, bool poss_dup) {
    store_le<std::uint32_t>(msg + hdr::kSeqNum, seq);
    msg[hdr::kPossDup] = poss_dup;
    store_le<std::uint32_t>(msg + len - kTrailerSize, checksum(msg, len - kTrailerSize));
  }

  // New outbound message carrying sequence number next_out_.
  void send_new(const std::uint8_t* msg, std::size_t len, std::uint64_t now) {
    assert(load_le<std::uint32_t>(msg + hdr::kSeqNum) == next_out_);
    tx_.send(msg, len);
    store_.append(next_out_++, msg, len);
    last_send_ = now;
  }

  void set_state(SessionState s, std::uint64_t now) {
    state_ = s;
    state_since_ = now;
  }

  void close(CloseReason r) {
    state_ = SessionState::Disconnected;
    tx_.close();
    app_.on_session_closed(r);
  }

  std::size_t fail(CloseReason r, std::size_t n) {
    close(r);
    return n;
  }

  void fatal_logout(CloseReason r, std::string_view text, std::uint64_t now) {
    send_new(scratch_, encode_logout(scratch_, next_out_, cfg_.comp_id, text), now);
    close(r);
  }

  SessionConfig cfg_;
  Transport& tx_;
  Handler& app_;
  SessionState state_ = SessionState::Disconnected;
  std::uint32_t next_out_ = 1;
  std::uint32_t next_in_ = 1;
  MessageStore store_;

  std::uint64_t state_since_ = 0;
  std::uint64_t last_send_ = 0;
  std::uint64_t last_recv_ = 0;
  bool test_req_pending_ = false;
  std::uint64_t test_req_sent_ = 0;
  std::uint16_t test_req_id_ = 0;

  bool resend_outstanding_ = false;
  std::uint32_t gap_max_ = 0;

  alignas(64) std::uint8_t scratch_[1024];
  alignas(64) std::uint8_t replay_[kMaxMessage];
  alignas(64) std::uint8_t rx_[kRxCapacity];
  std::size_t rx_len_ = 0;
};

}  // namespace obl::gw::ocgc
