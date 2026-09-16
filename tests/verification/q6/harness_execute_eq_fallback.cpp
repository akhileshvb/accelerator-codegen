// Build; Execute ≡ ExecuteFallback for ≤3 lineitems (Postgres Q6, 1-month window).
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>

#include "in_memory_store.hpp"
#include "nondet.hpp"
#include "row_sink.hpp"
#include "tpch/q6/postgres/aux/module.hpp"

using namespace accelerator_codegen::aux_state::core;
using accelerator_codegen::aux_state::query::TpchQ6Config;
using accelerator_codegen::aux_state::query::TpchQ6InitialScan;
using accelerator_codegen::aux_state::query::TpchQ6LineitemInput;
using accelerator_codegen::aux_state::query::TpchQ6Module;
using accelerator_codegen::aux_state::query::TpchQ6StartOfNextMonthDays;

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

class Services final : public AuxEngineServices, public TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    for (const auto &row : lineitems)
      consumer(row);
  }
  VisibilityToken CurrentVisibility() const override { return {}; }
  AuxStateStore &StateStore() override { return store; }

  accel_cbmc::InMemoryStore store{"tpch_q6"};
  std::vector<TpchQ6LineitemInput> lineitems;
};

void FillRow(TpchQ6LineitemInput &row, int salt, int32_t ship) {
#if defined(ACCEL_VERIFICATION_CONCRETE)
  row.shipdate_days = ship + (salt % 2);
  row.discount_raw = 6;
  row.quantity_raw = 1000 + salt * 100;
  row.extended_price_raw = 10000 + salt * 500;
#else
  (void)salt;
  row.shipdate_days = ship;
  row.discount_raw = accel_cbmc::NondetI64();
  row.quantity_raw = accel_cbmc::NondetI64();
  row.extended_price_raw = accel_cbmc::NondetI64();
  accel_cbmc::Assume(row.discount_raw >= 0 && row.discount_raw <= 10);
  accel_cbmc::Assume(row.quantity_raw >= 0 && row.quantity_raw <= 5000);
  accel_cbmc::Assume(row.quantity_raw % 100 == 0);
  accel_cbmc::Assume(row.extended_price_raw >= 100 &&
                     row.extended_price_raw <= 100000);
#endif
  row.discount_scale = row.quantity_scale = row.extended_price_scale = 2;
}

} // namespace

int main() {
  const int32_t month_start = DaysFromCivil(1994, 1, 1);
  const int32_t month_end = TpchQ6StartOfNextMonthDays(month_start);

  Services services;
  for (int i = 0; i < kN; ++i) {
    TpchQ6LineitemInput row;
    FillRow(row, i, month_start + 10);
    // Keep shipdates inside the month window.
    accel_cbmc::Assume(row.shipdate_days >= month_start &&
                       row.shipdate_days < month_end);
    services.lineitems.push_back(row);
  }

  TpchQ6Module module(TpchQ6Config{1}, &services);
  accel_cbmc::CollectingBuilder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));

  // One full month: CollectQueryPartitions emits a single month partition.
  QueryArguments args{
      {AuxValue::Of(AuxType::Date, AuxDate{month_start}),
       AuxValue::Of(AuxType::Double, 0.05),
       AuxValue::Of(AuxType::Double, 24.0)}};

  accel_cbmc::CollectingSink fast;
  module.Execute(services, args, fast);
  assert(fast.ended);

  accel_cbmc::CollectingSink fallback;
  module.ExecuteFallback(services, args, fallback);
  assert(fallback.ended);

  assert(accel_cbmc::IntegerColumnsEqual(fast, fallback));
  return 0;
}
