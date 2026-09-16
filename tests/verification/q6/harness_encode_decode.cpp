// Encode/decode round-trip for one Q6 revenue matrix (Postgres module).
#include <cassert>
#include <cstdint>

#include "nondet.hpp"
#include "tpch/q6/postgres/aux/finalize.hpp"
#include "tpch/q6/postgres/aux/module.hpp"

using accelerator_codegen::ApplyQ6CellContribution;
using accelerator_codegen::Q6RevenueMatrix;
using accelerator_codegen::Q6RowContributionRaw;
using accelerator_codegen::aux_state::query::DecodeTpchQ6Matrix;
using accelerator_codegen::aux_state::query::EncodeTpchQ6Matrix;

int main() {
  int8_t discount_cent;
  int64_t quantity_scaled;
  int64_t price_raw;

#if defined(ACCEL_VERIFICATION_CONCRETE)
  discount_cent = 6;
  quantity_scaled = 1000;
  price_raw = 10000;
#else
  discount_cent = static_cast<int8_t>(nondet_int());
  quantity_scaled = accel_cbmc::NondetI64();
  price_raw = accel_cbmc::NondetI64();
  accel_cbmc::Assume(discount_cent >= 0 && discount_cent <= 10);
  accel_cbmc::Assume(quantity_scaled >= 0 && quantity_scaled <= 5000);
  accel_cbmc::Assume(quantity_scaled % 100 == 0);
  accel_cbmc::Assume(price_raw >= 100 && price_raw <= 100000);
#endif

  Q6RevenueMatrix matrix;
  const auto contrib =
      Q6RowContributionRaw(price_raw, static_cast<int64_t>(discount_cent));
  ApplyQ6CellContribution(matrix, discount_cent, quantity_scaled, contrib, 1);

  const auto bytes = EncodeTpchQ6Matrix(matrix);
  Q6RevenueMatrix decoded;
  assert(DecodeTpchQ6Matrix(bytes, decoded));
  assert(decoded.At(discount_cent, quantity_scaled) ==
         matrix.At(discount_cent, quantity_scaled));
  return 0;
}
