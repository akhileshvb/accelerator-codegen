// Build(T); ApplyDelta(insert); Execute ≡ Build(T⊕m); Execute (Postgres Q6).
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
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

AuxRow ToAuxRow(const TpchQ6LineitemInput &in) {
  return {{AuxValue::Of(AuxType::Date, AuxDate{in.shipdate_days}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.discount_raw, in.discount_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.quantity_raw, in.quantity_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.extended_price_raw,
                                      in.extended_price_scale})}};
}

void FillRow(TpchQ6LineitemInput &row, int salt, int32_t ship) {
#if defined(ACCEL_VERIFICATION_CONCRETE)
  row.shipdate_days = ship;
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

void BuildPublish(TpchQ6Module &module, Services &services) {
  accel_cbmc::CollectingBuilder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
}

} // namespace

int main() {
  const int32_t month_start = DaysFromCivil(1994, 1, 1);
  const int32_t month_end = TpchQ6StartOfNextMonthDays(month_start);
  const int32_t ship = month_start + 10;

  Services services;
  for (int i = 0; i < kN; ++i) {
    TpchQ6LineitemInput row;
    FillRow(row, i, ship);
    services.lineitems.push_back(row);
  }

  // Same month so ApplyDelta finds the existing month partition.
  TpchQ6LineitemInput inserted;
  FillRow(inserted, kN, ship + 1);
  accel_cbmc::Assume(inserted.shipdate_days >= month_start &&
                     inserted.shipdate_days < month_end);

  QueryArguments args{
      {AuxValue::Of(AuxType::Date, AuxDate{month_start}),
       AuxValue::Of(AuxType::Double, 0.05),
       AuxValue::Of(AuxType::Double, 24.0)}};

  TpchQ6Module module_a(TpchQ6Config{1}, &services);
  BuildPublish(module_a, services);

  SetForcedMaintenanceActionName("ApplyDelta");
  AuxTransactionCoordinator coordinator;
  coordinator.Register(
      std::make_shared<TpchQ6Module>(TpchQ6Config{1}, &services));
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {ToAuxRow(inserted)};
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();
  SetForcedMaintenanceActionName("");

  services.lineitems.push_back(inserted);

  accel_cbmc::CollectingSink after_delta;
  module_a.Execute(services, args, after_delta);
  assert(after_delta.ended);

  Services services_b;
  services_b.lineitems = services.lineitems;
  TpchQ6Module module_b(TpchQ6Config{1}, &services_b);
  BuildPublish(module_b, services_b);

  accel_cbmc::CollectingSink after_rebuild;
  module_b.Execute(services_b, args, after_rebuild);
  assert(after_rebuild.ended);

  assert(accel_cbmc::IntegerColumnsEqual(after_delta, after_rebuild));
  return 0;
}
