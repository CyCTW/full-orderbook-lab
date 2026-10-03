// SPSC queue and byte ring across two threads; small ring so records wrap constantly.

#include <cstdint>
#include <cstring>
#include <thread>

#include "check.hpp"
#include "obl/gw/spsc.hpp"

using namespace obl::gw;

namespace {

void test_queue() {
  SpscQueue<std::uint64_t> q(64);
  constexpr std::uint64_t N = 2'000'000;
  std::thread prod([&] {
    for (std::uint64_t i = 0; i < N; ++i)
      while (!q.push(i)) std::this_thread::yield();
  });
  std::uint64_t expect = 0, bad = 0;
  while (expect < N) {
    if (auto v = q.pop()) bad += *v != expect++;
  }
  prod.join();
  CHECK_EQ(bad, 0u);
  CHECK(!q.pop().has_value());
}

void test_ring() {
  SpscRing r(4096);
  constexpr std::uint32_t N = 500'000;
  std::thread prod([&] {
    for (std::uint32_t i = 0; i < N; ++i) {
      const std::uint32_t size = 1 + i % 300;  // 1..300 bytes
      std::uint8_t* p;
      while (!(p = r.reserve(7, size))) std::this_thread::yield();
      for (std::uint32_t k = 0; k < size; ++k) p[k] = static_cast<std::uint8_t>(i + k);
      r.commit();
    }
  });
  std::uint32_t got = 0, bad = 0;
  while (got < N) {
    r.consume([&](std::uint32_t tag, const std::uint8_t* p, std::uint32_t size) {
      bad += tag != 7 || size != 1 + got % 300;
      for (std::uint32_t k = 0; k < size && !bad; ++k) bad += p[k] != static_cast<std::uint8_t>(got + k);
      ++got;
    });
  }
  prod.join();
  CHECK_EQ(bad, 0u);
  CHECK_EQ(got, N);
  // a record larger than the ring is refused
  CHECK(r.reserve(1, 5000) == nullptr);
}

}  // namespace

int main() {
  test_queue();
  test_ring();
  return test_result("test_spsc");
}
