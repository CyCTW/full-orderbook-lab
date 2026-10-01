#pragma once

#include <cstdio>
#include <cstdlib>

namespace obl::test {
inline int failures = 0;
}

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++obl::test::failures;                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

inline int test_result(const char* name) {
  if (obl::test::failures) {
    std::fprintf(stderr, "%s: %d failure(s)\n", name, obl::test::failures);
    return 1;
  }
  std::printf("%s: all checks passed\n", name);
  return 0;
}
