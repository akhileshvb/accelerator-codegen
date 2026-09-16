// Build(T); ApplyDelta(m); Execute ≡ Build(T⊕m); Execute  (Postgres Q1, N≤3, 1 insert)
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "accel_cbmc_bounds.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
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

AuxRow ToAuxRow(const TpchQ1LineitemInput &in) {
  return {{AuxValue::Of(AuxType::Date, AuxDate{in.shipdate_days}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.quantity_raw, in.quantity_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.extended_price_raw,
                                      in.extended_price_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.discount_raw, in.discount_scale}),
           AuxValue::Of(AuxType::ScaledDecimal,
                        ScaledDecimal{in.tax_raw, in.tax_scale}),
           AuxValue::Of(AuxType::String, std::string(1, in.returnflag)),
           AuxValue::Of(AuxType::String, std::string(1, in.linestatus))}};
}

char Pick(int idx, const char *opts, int n) {
  accel_cbmc::Assume(idx >= 0 && idx < n);
  return opts[idx];
}

void FillRow(TpchQ1LineitemInput &row, int salt) {
#if defined(ACCEL_VERIFICATION_CONCRETE)
  const int32_t base = DaysFromCivil(1995, 1, 15);
  row.shipdate_days = base + (salt % 2);
  row.quantity_raw = 1000 + salt * 100;
  row.extended_price_raw = 10000 + salt * 500;
  row.discount_raw = 5;
  row.tax_raw = 4;
  row.returnflag = 'N';
  row.linestatus = 'O';
#else
  (void)salt;
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
  row.returnflag = Pick(nondet_int() % 3, "ANR", 3);
  row.linestatus = Pick(nondet_int() % 2, "OF", 2);
#endif
  row.quantity_scale = row.extended_price_scale = row.discount_scale =
      row.tax_scale = 2;
}

void BuildPublish(TpchQ1Module &module, Services &services) {
  accel_cbmc::CollectingBuilder builder;
  module.Build(services, builder);
  assert(builder.finished);
  services.store.InstallBuild(std::move(builder.images));
}

} // namespace

int main() {
  Services services;
  accel_cbmc::ResetQ1ActiveDays();

  for (int i = 0; i < kN; ++i) {
    TpchQ1LineitemInput row;
    FillRow(row, i);
    services.lineitems.push_back(row);
    accel_cbmc::AddQ1ActiveDay(row.shipdate_days);
  }

  TpchQ1LineitemInput inserted;
  FillRow(inserted, kN);
  accel_cbmc::AddQ1ActiveDay(inserted.shipdate_days);

  int32_t day_offset;
#if defined(ACCEL_VERIFICATION_CONCRETE)
  day_offset = 90;
#else
  day_offset = accel_cbmc::NondetI32();
  accel_cbmc::Assume(day_offset >= 60 && day_offset <= 120);
#endif
  QueryArguments args{
      {AuxValue::Of(AuxType::Int64, static_cast<int64_t>(day_offset))}};

  // --- Path A: Build(T); ApplyDelta(insert); Execute ---
  TpchQ1Module module_a(TpchQ1Config{1}, &services);
  BuildPublish(module_a, services);

  SetForcedMaintenanceActionName("ApplyDelta");
  AuxTransactionCoordinator coordinator;
  auto mod_ptr = std::make_shared<TpchQ1Module>(TpchQ1Config{1}, &services);
  coordinator.Register(mod_ptr);
  coordinator.AttachServices(services);

  MutationBatch insert;
  insert.table = 1;
  insert.kind = MutationKind::Insert;
  insert.new_rows = {ToAuxRow(inserted)};
  coordinator.OnMutation(insert);
  coordinator.PreCommit();
  coordinator.Commit();
  SetForcedMaintenanceActionName("");

  // Keep table aligned with T⊕m for any fallback inside Execute.
  services.lineitems.push_back(inserted);

  accel_cbmc::CollectingSink after_delta;
  module_a.Execute(services, args, after_delta);
  assert(after_delta.ended);

  // --- Path B: Build(T⊕m); Execute ---
  Services services_b;
  services_b.lineitems = services.lineitems; // already includes insert
  for (const auto &row : services_b.lineitems)
    accel_cbmc::AddQ1ActiveDay(row.shipdate_days);

  TpchQ1Module module_b(TpchQ1Config{1}, &services_b);
  BuildPublish(module_b, services_b);

  accel_cbmc::CollectingSink after_rebuild;
  module_b.Execute(services_b, args, after_rebuild);
  assert(after_rebuild.ended);

  assert(accel_cbmc::IntegerColumnsEqual(after_delta, after_rebuild));
  return 0;
}
