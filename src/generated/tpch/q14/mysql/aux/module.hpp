#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/core/chunked_cow_array.hpp"
#include "accelerator_codegen/stateful/core/cow_fenwick_g1.hpp"
#include "tpch/q14/layout.hpp"

namespace accelerator_codegen::aux_state::query {

// State size and costs (one image, copy-on-write):
//  * part class: 1 B per part (ChunkedCowArray<uint8_t>, 64 Ki-id chunks;
//    absent / non-promo / promo) = 20 MB at SF100 (2e7 parts), 200 MB at
//    SF1000 (2e8 parts). The previous layout (dense array to 16M + an
//    unordered_map overflow) cost ~40 B per part beyond 16M ids.
//  * revenue: a Fenwick tree over shipdate days with {total, promo, count}
//    cells: ~2 x days nodes x 48 B = well under 1 MB, independent of SF.
// A lineitem insert/delete/update costs O(log days) node clones; a part
// insert/delete/type change costs one indexed point re-aggregation of that
// part's lineitems (WHERE l_partkey IN (...) GROUP BY day; PostgreSQL has
// idx_lineitem_partkey) and O(log days) node updates per affected day.
// Execute: <= 64 node visits, independent of SF.

struct TpchQ14Config {
  core::TableId part_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ14PartInput {
  int64_t key = 0;
  std::string type;
};

// Pre-aggregated (shipdate) revenue of lineitems that join a part, for
// SQL-pushdown builds. Revenue is sum(price * (1 - discount)) at scale 4.
struct TpchQ14DayAggregate {
  int32_t shipdate_days = 0;
  __int128 total_revenue_raw = 0;
  __int128 promo_revenue_raw = 0;
  int64_t count = 0;
};

// One part's lineitem revenue on one day (point re-aggregation by partkey).
struct TpchQ14PartDayRevenue {
  int64_t partkey = 0;
  int32_t shipdate_days = 0;
  __int128 revenue_raw = 0;
  int64_t count = 0;
};

class TpchQ14InitialScan {
public:
  virtual ~TpchQ14InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ14PartInput &)> &) = 0;
  // lineitem JOIN part GROUP BY shipdate, aggregated in the engine.
  virtual void ScanDayAggregates(
      const std::function<void(const TpchQ14DayAggregate &)> &) = 0;
  // lineitem WHERE l_partkey IN (partkeys) GROUP BY l_partkey, shipdate.
  virtual void ScanPartDayRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const TpchQ14PartDayRevenue &)> &) = 0;
};

enum class TpchQ14PromoClass : uint8_t {
  Absent = 0,   // no such part: its lineitems do not join
  NonPromo = 1,
  Promo = 2,
};

// One Fenwick cell: sums over the days it covers.
struct TpchQ14DayTotals {
  __int128 total_revenue_raw = 0;
  __int128 promo_revenue_raw = 0;
  int64_t count = 0; // joined lineitems (SUM over none is NULL)
};

struct TpchQ14State {
  core::ChunkedCowArray<uint8_t, 16> part_class{0};
  core::CowFenwick<TpchQ14DayTotals> days;

  TpchQ14PromoClass PromoOf(int64_t partkey) const {
    return static_cast<TpchQ14PromoClass>(part_class.Get(partkey));
  }
  void SetPromo(int64_t partkey, TpchQ14PromoClass promo) {
    part_class.Set(partkey, static_cast<uint8_t>(promo));
  }
  // Add `sign` x a (revenue, count) contribution of a part of class `promo`
  // on `day`.
  void AddRevenue(int32_t day, TpchQ14PromoClass promo, __int128 revenue,
                  int64_t count);
};

// The whole state is one image (partition 0).
struct TpchQ14StateImage {
  std::shared_ptr<const TpchQ14State> state;
};

struct TpchQ14LineitemDelta {
  int32_t shipdate_days = 0;
  int64_t partkey = 0;
  __int128 revenue_raw = 0;
  int8_t sign = 0;
};

// A part row appeared (class Promo / NonPromo) or went away (Absent).
struct TpchQ14PartEvent {
  int64_t partkey = 0;
  TpchQ14PromoClass promo = TpchQ14PromoClass::Absent;
};

bool TpchQ14IsPromoType(const std::string &type);
double TpchQ14FinalizePromoRevenue(const TpchQ14DayTotals &);

class TpchQ14TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ14LineitemDelta> &Deltas() const;
  const std::vector<TpchQ14PartEvent> &PartEvents() const;
  void MarkRebuild();
  void AddDelta(TpchQ14LineitemDelta delta);
  void AddPartEvent(TpchQ14PartEvent event);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ14LineitemDelta> deltas;
  std::vector<TpchQ14PartEvent> part_events;
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
  std::shared_ptr<TpchQ14State> BuildState(core::AuxEngineServices &) const;
  void Answer(const TpchQ14State &, const core::QueryArguments &,
              core::AuxRowSink &) const;
  void EmitResult(const TpchQ14DayTotals &, core::AuxRowSink &) const;

  TpchQ14Config config;
  TpchQ14InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
