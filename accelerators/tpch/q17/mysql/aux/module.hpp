#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/any_key_cow_array.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q17 layout ──────────────────────────────────────────────────────────────
// One published partition holds the whole state, built from copy-on-write
// chunks (a write batch copies pointers plus the chunks it touches):
//   * per part: its lineitem quantity histogram, i.e. a small sorted list of
//     (quantity, row count, price sum) entries,
//   * per part: the id of its (brand, container) group,
//   * per group: the exact answer so far (sum of l_extendedprice of the
//     qualifying lineitems and their count), kept incrementally.
// A query is a dictionary lookup of (brand, container) plus one array read.
//
// Memory: a histogram entry is 20 B (~one per distinct quantity of a part),
// plus ~60 B of per-part overhead (shared_ptr, vector header, group id, chunk
// directory): about 20 B per lineitem, ~12 GB at SF100 and ~120 GB at SF1000
// (the previous state held ~800 B per part transiently and copied whole
// 65,536-part chunks per write). Build streams a GROUP BY (partkey, quantity)
// ordered by partkey, so there is no transient copy of the histograms.
// Write batch: O(touched parts * (histogram + 64-part chunk) + groups touched);
// no table scan. Execute: O(1).
struct TpchQ17Layout {
  static constexpr const char *kAccelerator = "tpch_q17_aux";
  static constexpr core::PartitionId kStatePartition = 0;
};

// ── Config ──────────────────────────────────────────────────────────────────
struct TpchQ17Config {
  core::TableId part_table = 0;
  core::TableId lineitem_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
struct TpchQ17PartInput {
  int64_t key = 0;
  std::string brand;
  std::string container;
};

struct TpchQ17LineitemInput {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;       // decimal(15,2) raw (x100)
  int64_t extendedprice_raw = 0;  // decimal(15,2) raw (x100)
};

struct TpchQ17PartQuantityAggregate {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_sum_raw = 0;
  int64_t row_count = 0;
};

class TpchQ17InitialScan {
public:
  virtual ~TpchQ17InitialScan() = default;
  virtual void ScanParts(
      const std::function<void(const TpchQ17PartInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ17LineitemInput &)> &) = 0;

  // The same two scans split into `partitions` fixed row ranges: sink(partition,
  // row) runs concurrently for different partitions, one thread per partition.
  // The lineitem scan skips rows with a NULL partkey or quantity.
  virtual void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ17PartInput &)> &sink) = 0;
  virtual void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ17LineitemInput &)> &sink) = 0;

  // Optional SQL pre-aggregation: GROUP BY (partkey, quantity) with summed
  // price and row count, ORDER BY partkey, quantity. Returns false when the
  // engine does not implement it (the module then scans lineitems). Rows of one
  // part should arrive together; any other order is still handled, just slower.
  virtual bool ScanPartQuantityAggregates(
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &) {
    return false;
  }
};

// ── State ───────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
// One distinct l_quantity of a part.
struct TpchQ17QtyEntry {
  int64_t quantity_raw = 0;
  int64_t price_sum_raw = 0;
  uint32_t row_count = 0;
};
#pragma pack(pop)
static_assert(sizeof(TpchQ17QtyEntry) == 20);

// Sorted by quantity_raw, no empty entries.
using TpchQ17PartHistogram = std::vector<TpchQ17QtyEntry>;

struct TpchQ17GroupTotal {
  __int128 price_sum_raw = 0; // sum of l_extendedprice (x100) of qualifying rows
  int64_t row_count = 0;      // number of qualifying rows (NULL result when 0)
};

// (brand, container) -> dense group id. Exact (no hashing into slots).
struct TpchQ17GroupDictionary {
  std::unordered_map<std::string, uint32_t> ids;
  static std::string MakeKey(const std::string &brand,
                             const std::string &container);
};

constexpr uint32_t kTpchQ17NoGroup = 0xffffffffu;

// The published state. Copying it copies pointers only.
struct TpchQ17State {
  std::shared_ptr<const TpchQ17GroupDictionary> dictionary =
      std::make_shared<const TpchQ17GroupDictionary>();
  core::AnyKeyCowArray<TpchQ17GroupTotal, 8> totals;
  core::AnyKeyCowArray<uint32_t, 6> part_group{kTpchQ17NoGroup};
  core::AnyKeyCowArray<std::shared_ptr<const TpchQ17PartHistogram>, 6> parts;
};

struct TpchQ17StateImage {
  std::shared_ptr<const TpchQ17State> state;
};

// ── Delta ───────────────────────────────────────────────────────────────────
struct TpchQ17LineitemDelta {
  int64_t partkey = 0;
  int64_t quantity_raw = 0;
  int64_t extendedprice_raw = 0;
  int8_t sign = 0;
};

// A part row change: the old row (if any) is removed, then the new row (if
// any) is stored. A NULL brand or container means "in no group".
struct TpchQ17PartDelta {
  bool has_old = false;
  bool has_new = false;
  int64_t old_partkey = 0;
  int64_t new_partkey = 0;
  bool new_has_group = false;
  std::string new_brand;
  std::string new_container;
};

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ17TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ17LineitemDelta> &LineitemDeltas() const;
  const std::vector<TpchQ17PartDelta> &PartDeltas() const;
  void MarkRebuild();
  void AddLineitemDelta(TpchQ17LineitemDelta delta);
  void AddPartDelta(TpchQ17PartDelta delta);

private:
  bool pending = false;
  bool rebuild = false;
  std::vector<TpchQ17LineitemDelta> lineitem_deltas;
  std::vector<TpchQ17PartDelta> part_deltas;
};

// ── Module ──────────────────────────────────────────────────────────────────
class TpchQ17Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ17Layout::kAccelerator;

  explicit TpchQ17Module(TpchQ17Config config,
                         TpchQ17InitialScan *initial_scan = nullptr)
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
  // Streams the lineitem histograms through `consumer(partkey, histogram)`
  // (one call per part; a part may be reported more than once if the scan
  // delivers its rows non-contiguously).
  void ScanHistograms(
      const std::function<void(int64_t, TpchQ17PartHistogram &&)> &consumer)
      const;

  TpchQ17Config config;
  TpchQ17InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
