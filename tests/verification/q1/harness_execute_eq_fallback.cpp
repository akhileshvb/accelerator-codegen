// Build; Execute(p) ≡ ExecuteFallback(p) for ≤3 symbolic lineitems (Postgres Q1).
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "accel_cbmc_bounds.hpp"
#include "in_memory_store.hpp"
#include "nondet.hpp"
#include "row_sink.hpp"
#include "tpch/q1/postgres/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ1Config;
using accelerator_codegen::aux_state::query::TpchQ1InitialScan;
using accelerator_codegen::aux_state::query::TpchQ1LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ1Module;

namespace {

constexpr int32_t kEpoch = 719468;
constexpr int kN = 3;

int32_t DaysFromCivil(int32_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - kEpoch;
}

const int32_t kMinShip = DaysFromCivil(1992, 1, 2);
const int32_t kMaxShip = DaysFromCivil(1998, 12, 1);

class Services final : public AuxEngineServices, public TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer(row);
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  accel_cbmc::InMemoryStore store{"tpch_q1"};
  std::vector<TpchQ1LineitemInput> lineitems;
};

char Pick(int idx, const char *opts, int n) {
  accel_cbmc::Assume(idx >= 0 && idx < n);
  return opts[idx];
}

void FillSymbolicRow(TpchQ1LineitemInput &row) {
#if defined(ACCEL_VERIFICATION_CONCRETE)
  static int concrete_i = 0;
  const int32_t base = DaysFromCivil(1995, 1, 15);
  row.shipdate_days = base + (concrete_i % 2);
  row.quantity_raw = 1000 + concrete_i * 100;
  row.quantity_scale = 2;
  row.extended_price_raw = 10000 + concrete_i * 1000;
  row.extended_price_scale = 2;
  row.discount_raw = 5 + concrete_i;
  row.discount_scale = 2;
  row.tax_raw = 4;
  row.tax_scale = 2;
  row.returnflag = (concrete_i % 2 == 0) ? 'N' : 'A';
  row.linestatus = 'O';
  ++concrete_i;
#else
  row.shipdate_days = accel_cbmc::NondetI32();
  accel_cbmc::Assume(row.shipdate_days >= kMinShip &&
                     row.shipdate_days <= kMaxShip);
  row.quantity_raw = accel_cbmc::NondetI64();
  row.extended_price_raw = accel_cbmc::NondetI64();
  row.discount_raw = accel_cbmc::NondetI64();
  row.tax_raw = accel_cbmc::NondetI64();
  accel_cbmc::Assume(row.quantity_raw >= 100 && row.quantity_raw <= 5000);
  accel_cbmc::Assume(row.extended_price_raw >= 100 &&
                     row.extended_price_raw <= 100000);
  accel_cbmc::Assume(row.discount_raw >= 0 && row.discount_raw <= 10);
  accel_cbmc::Assume(row.tax_raw >= 0 && row.tax_raw <= 10);
  row.quantity_scale = row.extended_price_scale = row.discount_scale =
      row.tax_scale = 2;
  row.returnflag = Pick(nondet_int() % 3, "ANR", 3);
  row.linestatus = Pick(nondet_int() % 2, "OF", 2);
#endif
}

} // namespace

int main() {
  Services services;
  accel_cbmc::ResetQ1ActiveDays();

  for (int i = 0; i < kN; ++i) {
    TpchQ1LineitemInput row;
    FillSymbolicRow(row);
    services.lineitems.push_back(row);
    accel_cbmc::AddQ1ActiveDay(row.shipdate_days);
  }

  int32_t day_offset;
#if defined(ACCEL_VERIFICATION_CONCRETE)
  day_offset = 90;
#else
  day_offset = accel_cbmc::NondetI32();
  accel_cbmc::Assume(day_offset >= 60 && day_offset <= 120);
#endif

  // Ensure cutoff covers active days (kQ1BaseDays - offset >= max active).
  // Active days are ≤ kMaxShip; cutoff for offset∈[60,120] is near 1998 — OK.

  TpchQ1Module module(TpchQ1Config{1}, &services);
  accel_cbmc::CollectingBuilder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  QueryArguments args{{AuxValue::Of(AuxType::Int64, static_cast<int64_t>(day_offset))}};

  accel_cbmc::CollectingSink fast;
  module.Execute(services, args, fast);
  assert(fast.ended);

  accel_cbmc::CollectingSink fallback;
  module.ExecuteFallback(services, args, fallback);
  assert(fallback.ended);

  assert(accel_cbmc::IntegerColumnsEqual(fast, fallback));
  return 0;
}
