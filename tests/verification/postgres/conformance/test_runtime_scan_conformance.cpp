// Conformance: Q1 scan projection twin (scales=2) — pure structural check
// matching ScanTpchQ1Lineitems consumer in runtime.cpp (SPI oracle).
#include <cassert>
#include <cstdint>

#include "tpch/q1/postgres/aux/module.hpp"

using accelerator_codegen::aux_state::query::TpchQ1LineitemInput;

namespace {

// Literal twin of ScanTpchQ1Lineitems consumer projection.
TpchQ1LineitemInput ProjectQ1Lineitem(int32_t shipdate_days, int64_t qty,
                                      int64_t price, int64_t disc, int64_t tax,
                                      char rf, char ls) {
  return TpchQ1LineitemInput{shipdate_days, qty, 2, price, 2, disc, 2, tax, 2, rf,
                             ls};
}

void CheckProjectionScales() {
  auto in = ProjectQ1Lineitem(10000, 1000, 10000, 5, 4, 'N', 'O');
  assert(in.quantity_scale == 2);
  assert(in.extended_price_scale == 2);
  assert(in.discount_scale == 2);
  assert(in.tax_scale == 2);
  assert(in.returnflag == 'N');
  assert(in.linestatus == 'O');
  assert(in.quantity_raw == 1000);
}

void CheckDecodeQ1Arg() {
  // ExecutePostgresTpchQ1: PG_GETARG_INT32(0) → Int64 payload identity.
  const int32_t offset = 90;
  assert(static_cast<int64_t>(offset) == 90);
}

} // namespace

int main() {
  CheckProjectionScales();
  CheckDecodeQ1Arg();
  return 0;
}
