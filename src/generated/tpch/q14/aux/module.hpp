#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/query_layouts.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ14Config {
  core::TableId part_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ14PartInput {
  int64_t key = 0;
  std::string type;
};

struct TpchQ14LineitemInput {
  int64_t partkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
};

class TpchQ14InitialScan {
public:
  virtual ~TpchQ14InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ14LineitemInput &)> &) = 0;
};

enum class TpchQ14PromoClass : uint8_t {
  Absent = 0,
  NonPromo = 1,
  Promo = 2,
};

struct TpchQ14MonthTotals {
  int64_t total_revenue_raw = 0;
  int64_t promo_revenue_raw = 0;
  int64_t row_count = 0;
  bool saw_contribution = false;
};

struct TpchQ14State {
  static constexpr int64_t kMaxDensePartkey = 1 << 24; // 16M; SF20 is 4M
  std::vector<uint8_t> part_promo;
  std::unordered_map<int64_t, uint8_t> promo_overflow;
  std::map<int32_t, TpchQ14MonthTotals> month_totals;

  TpchQ14PromoClass PromoOf(int64_t partkey) const {
    if (partkey < 0)
      return TpchQ14PromoClass::Absent;
    const auto ov = promo_overflow.find(partkey);
    if (ov != promo_overflow.end())
      return static_cast<TpchQ14PromoClass>(ov->second);
    const auto id = static_cast<std::size_t>(partkey);
    if (id >= part_promo.size())
      return TpchQ14PromoClass::Absent;
    return static_cast<TpchQ14PromoClass>(part_promo[id]);
  }
  void SetPromo(int64_t partkey, TpchQ14PromoClass promo) {
    if (partkey < 0)
      return;
    if (partkey >= kMaxDensePartkey) {
      promo_overflow[partkey] = static_cast<uint8_t>(promo);
      return;
    }
    const auto id = static_cast<std::size_t>(partkey);
    if (part_promo.size() <= id)
      part_promo.resize(id + 1, 0);
    part_promo[id] = static_cast<uint8_t>(promo);
  }
};

enum class TpchQ14ImageKind : uint8_t { PartPromo, Month };

struct TpchQ14StateImage {
  TpchQ14ImageKind kind = TpchQ14ImageKind::PartPromo;
  int32_t month = 0;
  std::shared_ptr<const TpchQ14State> state;
};

struct TpchQ14LineitemDelta {
  int32_t month = 0;
  int64_t partkey = 0;
  int64_t revenue_raw = 0;
  bool is_promo = false;
  int8_t sign = 0;
};

std::vector<uint8_t> EncodeTpchQ14PartPromo(const TpchQ14State &);
std::vector<uint8_t> EncodeTpchQ14Month(int32_t month, const TpchQ14State &);
bool DecodeTpchQ14PartPromo(const std::vector<uint8_t> &, TpchQ14State &);
bool DecodeTpchQ14Month(const std::vector<uint8_t> &, int32_t month,
                        TpchQ14State &);
bool FindTpchQ14PartPromo(const std::vector<uint8_t> &, int64_t partkey,
                          TpchQ14PromoClass &);

bool TpchQ14IsPromoType(const std::string &type);
int64_t TpchQ14RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale);
int32_t TpchQ14MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ14DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ14AddMonths(int32_t days_since_epoch, int32_t months);
double TpchQ14FinalizePromoRevenue(const TpchQ14MonthTotals &);

class TpchQ14TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ14LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ14LineitemDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ14LineitemDelta> deltas;
};

class TpchQ14Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ14Layout::kAccelerator;

  explicit TpchQ14Module(TpchQ14Config config,
                         TpchQ14InitialScan *initial_scan = nullptr)
      : config(config), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override;
  std::vector<core::TableDependency> Dependencies() const override;
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override;
  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &) const override;
  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override;
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;
  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;

private:
  TpchQ14State BuildState(core::AuxEngineServices &) const;
  bool LoadMonth(core::AuxEngineServices &, int32_t month,
                 TpchQ14MonthTotals &) const;
  void EmitResult(const TpchQ14MonthTotals &, core::AuxRowSink &) const;

  TpchQ14Config config;
  TpchQ14InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
