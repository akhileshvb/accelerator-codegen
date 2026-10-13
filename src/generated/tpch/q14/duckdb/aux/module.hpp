#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/prims/chunked_cow_array.hpp"
#include "tpch/q14/layout.hpp"

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

int32_t TpchQ14MonthKeyFromDays(int32_t days_since_epoch);
bool TpchQ14IsPromoType(const std::string &type);
int64_t TpchQ14RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale);

class TpchQ14InitialScan {
public:
  virtual ~TpchQ14InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ14LineitemInput &)> &) = 0;
  virtual void ScanLineitemsForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ14LineitemInput &)> &consumer) {
    ScanLineitems([&](const TpchQ14LineitemInput &input) {
      if (months.count(TpchQ14MonthKeyFromDays(input.shipdate_days)))
        consumer(input);
    });
  }
  // (partkey, is PROMO type) for every part.
  virtual void ScanPartPromo(
      const std::function<void(int64_t, bool)> &consumer) {
    ScanParts([&](const TpchQ14PartInput &input) {
      consumer(input.key, TpchQ14IsPromoType(input.type));
    });
  }
  // Pushed-down GROUP BY month over lineitem JOIN part: (month, total
  // revenue raw, promo revenue raw, row count). `months` empty means every
  // month. Returns false when the source cannot aggregate (callers then scan
  // rows).
  virtual bool ScanMonthAggregates(
      const std::set<int32_t> &,
      const std::function<void(int32_t, int64_t, int64_t, int64_t)> &) {
    return false;
  }
  // Per (partkey, month): (revenue raw, row count) of the lineitems of the
  // given parts.
  virtual void ScanPartMonthRevenue(
      const std::vector<int64_t> &keys,
      const std::function<void(int64_t, int32_t, int64_t, int64_t)> &consumer) {
    const std::unordered_set<int64_t> wanted(keys.begin(), keys.end());
    std::map<std::pair<int64_t, int32_t>, std::pair<int64_t, int64_t>> acc;
    ScanLineitems([&](const TpchQ14LineitemInput &input) {
      if (!wanted.count(input.partkey))
        return;
      auto &cell = acc[{input.partkey,
                        TpchQ14MonthKeyFromDays(input.shipdate_days)}];
      cell.first += TpchQ14RevenueRaw(input.extended_price_raw,
                                      input.extended_price_scale,
                                      input.discount_raw, input.discount_scale);
      cell.second += 1;
    });
    for (const auto &[k, v] : acc)
      consumer(k.first, k.second, v.first, v.second);
  }
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

// Part -> promo class, in 64 KiB copy-on-write chunks so a point write copies
// one chunk plus the chunk directory (published states are never mutated; a
// copy of the state shares every chunk until SetRaw replaces the touched one).
struct TpchQ14State {
  static constexpr int64_t kMaxDensePartkey = 1 << 24; // 16M; SF20 is 4M
  static constexpr uint8_t kClassMask = 3;
  // Part inserted after the build and not yet referenced by any lineitem
  // delta: deleting/retyping it needs no lineitem scan.
  static constexpr uint8_t kFreshBit = 4;
  // Absent keys only: lineitems of the key may exist (a re-insert must add
  // them) / the key was deleted while known to have none.
  static constexpr uint8_t kGoneBit = 8;
  static constexpr uint8_t kDeadBit = 16;
  // Largest partkey present at build; larger absent keys never had rows.
  int64_t build_max_partkey = std::numeric_limits<int64_t>::max();
  core::ChunkedCowArray<uint8_t, 16> classes{};
  std::unordered_map<int64_t, uint8_t> promo_overflow;
  std::map<int32_t, TpchQ14MonthTotals> month_totals;

  uint8_t RawOf(int64_t partkey) const {
    if (partkey < 0)
      return 0;
    if (partkey >= kMaxDensePartkey) {
      const auto ov = promo_overflow.find(partkey);
      return ov == promo_overflow.end() ? 0 : ov->second;
    }
    return classes.Get(partkey);
  }
  TpchQ14PromoClass PromoOf(int64_t partkey) const {
    return static_cast<TpchQ14PromoClass>(RawOf(partkey) & kClassMask);
  }
  void SetRaw(int64_t partkey, uint8_t raw) {
    if (partkey < 0)
      return;
    if (partkey >= kMaxDensePartkey) {
      promo_overflow[partkey] = raw;
      return;
    }
    classes.Set(partkey, raw);
  }
  void SetPromo(int64_t partkey, TpchQ14PromoClass promo) {
    SetRaw(partkey, static_cast<uint8_t>(promo));
  }
};

enum class TpchQ14ImageKind : uint8_t { PartPromo, Month };

// PartPromo images carry the part-class state; Month images carry their
// totals inline (one small allocation per published month).
struct TpchQ14StateImage {
  TpchQ14ImageKind kind = TpchQ14ImageKind::PartPromo;
  int32_t month = 0;
  std::shared_ptr<const TpchQ14State> state;
  TpchQ14MonthTotals totals;
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

int32_t TpchQ14DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ14AddMonths(int32_t days_since_epoch, int32_t months);
double TpchQ14FinalizePromoRevenue(const TpchQ14MonthTotals &);

struct TpchQ14PartEvent {
  core::MutationKind kind = core::MutationKind::Insert;
  int64_t key = 0;
  TpchQ14PromoClass new_class = TpchQ14PromoClass::Absent;
};

class TpchQ14TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ14LineitemDelta> &Deltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ14LineitemDelta delta);
  const std::vector<TpchQ14PartEvent> &PartEvents() const;
  void AddPartEvent(TpchQ14PartEvent event);

private:
  std::vector<TpchQ14PartEvent> part_events;
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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ14State BuildState(core::AuxEngineServices &) const;
  bool LoadMonth(core::AuxEngineServices &, int32_t month,
                 TpchQ14MonthTotals &) const;
  void EmitResult(const TpchQ14MonthTotals &, core::AuxRowSink &) const;

  TpchQ14Config config;
  TpchQ14InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
