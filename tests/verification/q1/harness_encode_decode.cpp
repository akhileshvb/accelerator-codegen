// Encode/decode round-trip for one Q1 day partition (Postgres module).
#include <cassert>
#include <cstdint>
#include <string>

#include "accel_cbmc_bounds.hpp"
#include "nondet.hpp"
#include "tpch/q1/postgres/aux/finalize.hpp"
#include "tpch/q1/postgres/aux/module.hpp"

using accelerator_codegen::ApplyQ1RowContribution;
using accelerator_codegen::PackQ1GroupKey;
using accelerator_codegen::Q1DayGroupStats;
using accelerator_codegen::Q1DayPartition;
using accelerator_codegen::Q1GroupKey;
using accelerator_codegen::aux_state::query::DecodeTpchQ1Day;
using accelerator_codegen::aux_state::query::EncodeTpchQ1Day;

namespace {

bool GroupEqual(const Q1DayGroupStats &a, const Q1DayGroupStats &b) {
  return a.count == b.count && a.sum_qty == b.sum_qty &&
         a.sum_base_price == b.sum_base_price &&
         a.sum_disc_price == b.sum_disc_price && a.sum_charge == b.sum_charge &&
         a.sum_discount == b.sum_discount;
}

char FlagFromIndex(int idx, const char *opts, int n) {
  accel_cbmc::Assume(idx >= 0 && idx < n);
  return opts[idx];
}

} // namespace

int main() {
  int64_t qty, price, disc, tax;
  char rf, ls;

#if defined(ACCEL_VERIFICATION_CONCRETE)
  qty = 1000;
  price = 10000;
  disc = 10;
  tax = 5;
  rf = 'N';
  ls = 'O';
#else
  qty = accel_cbmc::NondetI64();
  price = accel_cbmc::NondetI64();
  disc = accel_cbmc::NondetI64();
  tax = accel_cbmc::NondetI64();
  accel_cbmc::Assume(qty >= 100 && qty <= 5000);
  accel_cbmc::Assume(price >= 100 && price <= 100000);
  accel_cbmc::Assume(disc >= 0 && disc <= 10);
  accel_cbmc::Assume(tax >= 0 && tax <= 10);
  rf = FlagFromIndex(nondet_int() % 3, "ANR", 3);
  ls = FlagFromIndex(nondet_int() % 2, "OF", 2);
#endif

  Q1DayPartition day;
  auto &group = day.groups[PackQ1GroupKey(rf, ls)];
  ApplyQ1RowContribution(group, qty, price, disc, tax, 1);

  const auto bytes = EncodeTpchQ1Day(day);
  Q1DayPartition decoded;
  assert(DecodeTpchQ1Day(bytes, decoded));
  assert(decoded.groups.size() == day.groups.size());
  for (const auto &entry : day.groups) {
    auto it = decoded.groups.find(entry.first);
    assert(it != decoded.groups.end());
    assert(GroupEqual(entry.second, it->second));
  }
  return 0;
}
