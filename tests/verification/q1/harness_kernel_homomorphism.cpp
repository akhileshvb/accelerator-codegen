// Bounded verification: Q1 finalize kernels are a homomorphism under Apply/Merge.
// Property: applying two row contributions then merging day stats is order-independent;
// insert then compensating delete yields a zero group.
#include <cassert>
#include <cstdint>

#include "nondet.hpp"
#include "tpch/q1/postgres/aux/finalize.hpp"

using accelerator_codegen::ApplyQ1RowContribution;
using accelerator_codegen::MergeQ1DayGroupStats;
using accelerator_codegen::PackQ1GroupKey;
using accelerator_codegen::Q1DayGroupStats;
using accelerator_codegen::Q1MergedGroupStats;

namespace {

bool StatsEqual(const Q1DayGroupStats &a, const Q1DayGroupStats &b) {
  return a.count == b.count && a.sum_qty == b.sum_qty &&
         a.sum_base_price == b.sum_base_price &&
         a.sum_disc_price == b.sum_disc_price && a.sum_charge == b.sum_charge &&
         a.sum_discount == b.sum_discount;
}

bool MergedEqual(const Q1MergedGroupStats &a, const Q1MergedGroupStats &b) {
  return a.count == b.count && a.sum_qty == b.sum_qty &&
         a.sum_base_price == b.sum_base_price &&
         a.sum_disc_price == b.sum_disc_price && a.sum_charge == b.sum_charge &&
         a.sum_discount == b.sum_discount;
}

void BoundRaw(int64_t &v, int64_t lo, int64_t hi) {
  accel_cbmc::Assume(v >= lo && v <= hi);
}

} // namespace

int main() {
  int64_t qty_a, price_a, disc_a, tax_a;
  int64_t qty_b, price_b, disc_b, tax_b;

#if defined(ACCEL_VERIFICATION_CONCRETE)
  qty_a = 1000;
  price_a = 10000;
  disc_a = 10;
  tax_a = 5;
  qty_b = 2000;
  price_b = 20000;
  disc_b = 6;
  tax_b = 8;
#else
  qty_a = accel_cbmc::NondetI64();
  price_a = accel_cbmc::NondetI64();
  disc_a = accel_cbmc::NondetI64();
  tax_a = accel_cbmc::NondetI64();
  qty_b = accel_cbmc::NondetI64();
  price_b = accel_cbmc::NondetI64();
  disc_b = accel_cbmc::NondetI64();
  tax_b = accel_cbmc::NondetI64();
  BoundRaw(qty_a, 100, 5000);
  BoundRaw(price_a, 100, 100000);
  BoundRaw(disc_a, 0, 10);
  BoundRaw(tax_a, 0, 10);
  BoundRaw(qty_b, 100, 5000);
  BoundRaw(price_b, 100, 100000);
  BoundRaw(disc_b, 0, 10);
  BoundRaw(tax_b, 0, 10);
#endif

  // Order independence of Apply on the same group.
  Q1DayGroupStats ab{};
  ApplyQ1RowContribution(ab, qty_a, price_a, disc_a, tax_a, 1);
  ApplyQ1RowContribution(ab, qty_b, price_b, disc_b, tax_b, 1);

  Q1DayGroupStats ba{};
  ApplyQ1RowContribution(ba, qty_b, price_b, disc_b, tax_b, 1);
  ApplyQ1RowContribution(ba, qty_a, price_a, disc_a, tax_a, 1);
  assert(StatsEqual(ab, ba));

  // Insert + delete cancels.
  Q1DayGroupStats cancel{};
  ApplyQ1RowContribution(cancel, qty_a, price_a, disc_a, tax_a, 1);
  ApplyQ1RowContribution(cancel, qty_a, price_a, disc_a, tax_a, -1);
  assert(cancel.count == 0);
  assert(cancel.sum_qty == 0);
  assert(cancel.sum_base_price == 0);
  assert(cancel.sum_disc_price == 0);
  assert(cancel.sum_charge == 0);
  assert(cancel.sum_discount == 0);

  // Merge is addition: Merge(A)+Merge(B) on separate day stats.
  Q1DayGroupStats day1{};
  Q1DayGroupStats day2{};
  ApplyQ1RowContribution(day1, qty_a, price_a, disc_a, tax_a, 1);
  ApplyQ1RowContribution(day2, qty_b, price_b, disc_b, tax_b, 1);

  Q1MergedGroupStats merged_ab{};
  MergeQ1DayGroupStats(merged_ab, day1);
  MergeQ1DayGroupStats(merged_ab, day2);

  Q1MergedGroupStats merged_ba{};
  MergeQ1DayGroupStats(merged_ba, day2);
  MergeQ1DayGroupStats(merged_ba, day1);
  assert(MergedEqual(merged_ab, merged_ba));

  // Pack key is a pure function of flags.
  const auto key = PackQ1GroupKey('A', 'F');
  assert(key == PackQ1GroupKey('A', 'F'));
  (void)key;

  return 0;
}
