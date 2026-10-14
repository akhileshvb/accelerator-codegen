#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/any_key_cow_array.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ15Layout {
  static constexpr const char *kAccelerator = "tpch_q15";
  static constexpr core::PartitionId kSupplierDirectory = 0;
  // Month partitions are keyed by sql_date::MonthIndex (year*12+month-1) plus a
  // fixed bias so every month of any PostgreSQL date is a positive id.
};

struct TpchQ15Config {
  core::TableId lineitem_table = 0;
  core::TableId supplier_table = 0;
};

struct TpchQ15LineitemInput {
  int32_t shipdate_days = 0;
  int64_t suppkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
};

struct TpchQ15SupplierInput {
  int64_t suppkey = 0;
  std::string name;
  std::string address;
  std::string phone;
};

class TpchQ15InitialScan {
public:
  virtual ~TpchQ15InitialScan() = default;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ15LineitemInput &)> &) = 0;
  virtual void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const TpchQ15LineitemInput &)> &consumer) {
    ScanLineitems([&](const TpchQ15LineitemInput &input) {
      if (input.shipdate_days >= start_days && input.shipdate_days < end_days)
        consumer(input);
    });
  }
  virtual void ScanSuppliers(
      const std::function<void(const TpchQ15SupplierInput &)> &) = 0;
};

// ── State ─────────────────────────────────────────────────────────────────
//
// Layout (all published partitions are immutable; copies share COW chunks):
//   * kSupplierDirectory: suppkey -> packed (name, address, phone), a
//     AnyKeyCowArray of std::string (empty string = no such supplier).
//   * one partition per calendar month: per-supplier revenue and row count for
//     the WHOLE month (dense per-key columns, COW chunks), plus per-day
//     (suppkey, revenue) lists for the day-granular edges of a window.
// A query window [d, d + 3 months) is answered exactly for every d: whole
// months come from the dense columns, partial months from the day lists, then
// one sum/max sweep over suppliers. No winners or supplier attributes are
// cached; the supplier directory is joined at emit time.
//
// Memory: dense columns 12 B per (month, supplier) slot (touched chunks only;
// TPC-H has ~84 months), day lists 8 B per lineitem (u32 suppkey, u32 revenue;
// anything that does not fit goes to a 24 B "wide" entry). That is ~10 B per
// lineitem: ~6 GB at SF100, ~60 GB at SF1000 (the old per-window hash maps
// were ~3.4 GB / 34 GB at 3 windows per row but copied whole per write batch).
// Write batch: O(touched months * (dense chunk dir + 31 day lists) + touched
// chunks + delta rows); the day-list overlay is compacted when it grows past
// 1/16 of its base. Execute: O(edge rows + suppliers) per call (about 4 ns per
// supplier slot, ~40 ms at SF1000 for a whole-month window).

// One lineitem (or net group of lineitems) of a day whose values do not fit the
// narrow 8-byte encoding, or a signed overlay delta.
struct TpchQ15DayEntry {
  int64_t suppkey = 0;
  int64_t revenue_raw = 0; // scale 4
  int64_t count = 0;       // number of lineitems (negative: removals)
};

// Per-day sparse (suppkey, revenue) list: immutable base + small signed overlay.
struct TpchQ15DayList {
  // (suppkey << 32 | revenue), each one lineitem.
  std::shared_ptr<const std::vector<uint64_t>> narrow;
  std::shared_ptr<const std::vector<TpchQ15DayEntry>> wide;
  std::shared_ptr<const std::vector<TpchQ15DayEntry>> overlay;
};

constexpr int64_t kTpchQ15WideRevenue = std::numeric_limits<int64_t>::min();

struct TpchQ15MonthState {
  // Whole-month revenue per supplier; kTpchQ15WideRevenue means the exact
  // value lives in wide_revenue.
  core::AnyKeyCowArray<int64_t> revenue{0};
  core::AnyKeyCowArray<int32_t> count{0};
  std::map<int64_t, __int128> wide_revenue;
  std::array<TpchQ15DayList, 31> days;
};

struct TpchQ15SupplierDirectory {
  core::AnyKeyCowArray<std::string> entries;
};

enum class TpchQ15ImageKind : uint8_t {
  SupplierDirectory,
  MonthWindow,
};

struct TpchQ15StateImage {
  TpchQ15ImageKind kind = TpchQ15ImageKind::MonthWindow;
  int32_t month_index = 0;
  std::shared_ptr<const TpchQ15SupplierDirectory> directory;
  std::shared_ptr<const TpchQ15MonthState> month;
};

struct TpchQ15LineitemDelta {
  int32_t shipdate_days = 0;
  int64_t suppkey = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

// One supplier row change: the old row (if any) is removed, then the new row
// (if any) is stored under its own key.
struct TpchQ15SupplierDelta {
  int64_t old_suppkey = 0;
  int64_t new_suppkey = 0;
  bool has_old = false;
  bool has_new = false;
  std::string new_name;
  std::string new_address;
  std::string new_phone;
};

// ── Helpers ───────────────────────────────────────────────────────────────

int32_t TpchQ15MonthKeyFromDays(int32_t days_since_epoch);
int32_t TpchQ15DaysFromYmd(int32_t year, unsigned month, unsigned day);
bool TpchQ15IsFirstOfMonth(int32_t days_since_epoch);
int32_t TpchQ15AddMonths(int32_t days_since_epoch, int32_t months);
int64_t TpchQ15RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                          int64_t discount_raw, int32_t discount_scale);

// ── TxnBuffer ─────────────────────────────────────────────────────────────

class TpchQ15TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ15LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ15SupplierDelta> &SupplierDeltas() const;
  void MarkRebuild();
  void AddLineitemDelta(TpchQ15LineitemDelta delta);
  void AddSupplierDelta(TpchQ15SupplierDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ15LineitemDelta> lineitem_deltas;
  std::vector<TpchQ15SupplierDelta> supplier_deltas;
};

// ── Module ────────────────────────────────────────────────────────────────

class TpchQ15Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ15Layout::kAccelerator;

  explicit TpchQ15Module(TpchQ15Config config,
                         TpchQ15InitialScan *initial_scan = nullptr)
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
  static void EmitEmpty(core::AuxRowSink &);

  TpchQ15Config config;
  TpchQ15InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
