// Gateway over real TCP (loopback): Lookup Service with failover between endpoints, RSA
// encrypted Logon, orders, and failover from the primary to the mirror trading service.

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "check.hpp"
#include "obl/gw/net/tcp.hpp"
#include "obl/gw/ocgc/connector.hpp"
#include "obl/gw/ocgc/gateway.hpp"
#include "obl/gw/ocgc/password.hpp"
#include "obl/gw/ocgc/sim/server.hpp"

using namespace obl;
using namespace obl::gw;
using namespace obl::gw::ocgc;

namespace {

constexpr std::int64_t S = kScale;
constexpr std::uint64_t kMs = 1'000'000;

struct Recorder {
  int acks = 0, fills = 0, ups = 0, downs = 0;
  void on_order_update(const OrderUpdate& u, const Order&) {
    acks += u.event == OrderEvent::Acked;
    fills += u.event == OrderEvent::Filled;
  }
  void on_request_dropped(OrderSlot, ReqKind, DropReason) {}
  void on_session(bool up, CloseReason) { (up ? ups : downs)++; }
  void on_kill_switch(KillReason) {}
  void on_mass_cancel(const MassCancelReport&) {}
};

using Gw = OcgcGateway<ConnTransport, Recorder>;

#if defined(OBL_HAVE_OPENSSL)
struct Keys {
  std::string pub, priv;
};
Keys make_keys() {
  EVP_PKEY* k = EVP_RSA_gen(2048);
  Keys out;
  BIO* b = BIO_new(BIO_s_mem());
  PEM_write_bio_PUBKEY(b, k);
  char* p = nullptr;
  long n = BIO_get_mem_data(b, &p);
  out.pub.assign(p, static_cast<std::size_t>(n));
  BIO_free(b);
  b = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
  n = BIO_get_mem_data(b, &p);
  out.priv.assign(p, static_cast<std::size_t>(n));
  BIO_free(b);
  EVP_PKEY_free(k);
  return out;
}

void test_password_crypto(const Keys& k) {
  const std::time_t t = 1'790'000'000;
  auto field = encrypt_password(k.pub, "s3cret", t);
  CHECK(field.has_value());
  CHECK_EQ(field->size(), 344u);  // base64 of 256 bytes
  auto d = decrypt_password(k.priv, *field);
  CHECK(d.has_value());
  CHECK(d->password == "s3cret");
  CHECK_EQ(d->login_utc, t);
  CHECK(utc_stamp(t) == "20260921141320");
  auto oaep = encrypt_password(k.pub, "x", t, RsaPadding::Oaep);
  CHECK(oaep && decrypt_password(k.priv, *oaep, RsaPadding::Oaep)->password == "x");
  CHECK(!decrypt_password(k.priv, *oaep, RsaPadding::Pkcs1).has_value());
}
#endif

void test_tcp_end_to_end() {
  sim::ExchangeConfig xc;
#if defined(OBL_HAVE_OPENSSL)
  const Keys keys = make_keys();
  test_password_crypto(keys);
  // the simulator decrypts like OCG-C and checks the login time within 30 s
  xc.verify_password = [&keys](std::string_view field, const std::string& expected, std::uint64_t now) {
    auto d = decrypt_password(keys.priv, field);
    if (!d || d->password != expected) return false;
    const long long skew = static_cast<long long>(now / 1'000'000'000) - static_cast<long long>(d->login_utc);
    return skew > -30 && skew < 30;
  };
#endif
  sim::Exchange ex(xc);
  ex.add_symbol("700");
  ex.add_client("CO1", "s3cret");
  sim::Server srv(ex);
  CHECK(srv.start());

  // an endpoint that refuses connections: the Lookup client must move on to the next one
  net::TcpListener dead;
  dead.listen("127.0.0.1", 0);
  const std::uint16_t dead_port = dead.port();
  dead.close();

  InstrumentTable ins;
  const SymbolIdx t = ins.add({"700", 100 * S, true, false, 400 * S});
  OrderTableConfig oc;
  oc.max_requests = 1 << 14;
  oc.max_orders = 1 << 12;
  oc.max_symbols = 8;
  OrderTable orders(oc);
  RiskEngine risk(ins, orders, 8);
  Recorder app;
  ConnTransport tx;

  GatewayConfig g;
  g.session.comp_id = "CO1";
#if defined(OBL_HAVE_OPENSSL)
  g.session.password_provider = [&keys] {
    return *encrypt_password(keys.pub, "s3cret", static_cast<std::time_t>(net::wall_ns() / 1'000'000'000));
  };
#else
  g.session.encrypted_password = "s3cret";
#endif
  g.session.store_reserve_bytes = 1 << 20;
  g.session.store_reserve_msgs = 1 << 12;
  g.broker_id = "1234";
  g.bcan = "ABC123.2568";
  g.trade_date = 20261003;
  Gw gw(g, tx, app, ins, orders, risk);

  ConnectorConfig cc;
  cc.comp_id = "CO1";
  cc.lookup = {{"127.0.0.1", dead_port}, {"127.0.0.1", srv.lookup_port()}};
  cc.lookup_retry_ns = 20 * kMs;
  cc.reconnect_delay_ns = 50 * kMs;
  cc.connect_timeout_ns = 500 * kMs;
  Connector<Gw> conn(cc, gw, tx);

  auto run_until = [&](const std::function<bool()>& done, std::uint64_t timeout_ms) {
    const std::uint64_t end = net::wall_ns() + timeout_ms * kMs;
    while (net::wall_ns() < end) {
      const std::uint64_t now = net::wall_ns();
      srv.step(now);
      conn.step(now);
      if (done()) return true;
    }
    return false;
  };

  conn.start(net::wall_ns());
  CHECK(run_until([&] { return gw.active(); }, 3000));
  CHECK_EQ(conn.lookup_index(), 1);  // first endpoint was dead
  CHECK_EQ(conn.target(), 0);        // primary trading service
  CHECK_EQ(app.ups, 1);

  auto a = gw.new_order(t, OrdSide::Buy, 399 * S, 100 * S, net::wall_ns());
  CHECK(a.status == SendStatus::Sent);
  CHECK(run_until([&] { return orders.order(a.slot).state == OrdState::New; }, 2000));

  ex.add_liquidity("700", 2, 400 * S, 100 * S);
  auto b = gw.new_order(t, OrdSide::Buy, 400 * S, 100 * S, net::wall_ns());
  CHECK(run_until([&] { return orders.order(b.slot).state == OrdState::Filled; }, 2000));
  CHECK_EQ(orders.position(t).pos, 100 * S);

  // primary goes down: reconnect to primary fails, mirror takes over, sequence numbers carry on
  srv.fail_primary();
  CHECK(run_until([&] { return !gw.active(); }, 2000));
  CHECK(run_until([&] { return gw.active(); }, 5000));
  CHECK_EQ(conn.target(), 1);
  CHECK_EQ(app.downs, 1);
  CHECK_EQ(app.ups, 2);
  CHECK_EQ(gw.session().next_in_seq(), ex.next_out("CO1"));
  CHECK_EQ(gw.session().next_out_seq(), ex.next_in("CO1"));
  CHECK(gw.cancel(a.slot, net::wall_ns()) == SendStatus::Sent);  // order from before the failover
  CHECK(run_until([&] { return orders.order(a.slot).state == OrdState::Cancelled; }, 2000));

  // end of day
  conn.stop();
  gw.logout(net::wall_ns());
  CHECK(run_until([&] { return conn.state() == Connector<Gw>::State::Idle; }, 2000));
  CHECK(!gw.active());
}

}  // namespace

int main() {
  test_tcp_end_to_end();
  return test_result("test_tcp");
}
