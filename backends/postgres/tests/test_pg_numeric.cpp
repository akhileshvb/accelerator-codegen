// Unit test of the PostgreSQL NUMERIC helpers: pure C++, no server needed.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "backends/postgres/api/pg_numeric_div.hpp"

#define CHECK(c)                                                                         \
  do {                                                                                   \
    if (!(c)) {                                                                          \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);          \
      std::abort();                                                                      \
    }                                                                                    \
  } while (0)

static void TestDivRound() {
  using namespace accelerator_codegen::pg_numeric;
  using accelerator_codegen::aux_state::core::decimal_text::Decimal;
  using accelerator_codegen::aux_state::core::decimal_text::Int128;
  Decimal q = DivRound(6, 0, 3, 0);
  CHECK(q.scale >= 0 && q.coeff == Int128(2) * [&] { Int128 p = 1; for (int i = 0; i < q.scale; ++i) p *= 10; return p; }());
  q = DivRound(1, 0, 8, 0);   // 0.125 exactly
  { Int128 p = 1; for (int i = 0; i < q.scale - 3; ++i) p *= 10; CHECK(q.scale >= 3 && q.coeff == 125 * p); }
  q = DivRound(2, 0, 3, 0);   // 0.666..67, rounded half away from zero
  CHECK(q.coeff % 10 == 7);
  q = DivRound(-2, 0, 3, 0);
  CHECK(q.coeff % 10 == -7 && q.coeff < 0);
  q = DivRound(12345, 2, 5, 0); // 123.45 / 5 = 24.69
  { Int128 p = 1; for (int i = 0; i < q.scale - 2; ++i) p *= 10; CHECK(q.scale >= 2 && q.coeff == 2469 * p); }
  q = DivRound(0, 0, 7, 0);
  CHECK(q.coeff == 0);
  bool threw = false;
  try { DivRound(1, 0, 0, 0); } catch (const std::domain_error &) { threw = true; }
  CHECK(threw);
}


static void TestAvg() {
  using namespace accelerator_codegen::pg_numeric;
  // avg(1, 2) = 1.5: PostgreSQL gives a result of 16 fraction digits.
  const auto avg = AvgOfScaledSum(3, 0, 2);
  CHECK(avg.scale == 16);
  CHECK(accelerator_codegen::aux_state::core::decimal_text::ToText(avg) == "1.5000000000000000");
}

int main() {
  TestDivRound();
  TestAvg();
  std::puts("test_pg_numeric: PASS");
  return 0;
}
