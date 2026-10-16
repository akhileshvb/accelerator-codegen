#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/cow_record_store.hpp"
#include "prims/flat_hash_map.hpp"

namespace accelerator_codegen::aux_state::query {

// ── Q3 layout ───────────────────────────────────────────────────────────────
struct TpchQ3Layout {
  static constexpr const char *kAccelerator = "tpch_q3";
  static constexpr core::PartitionId kState = 0;

  static constexpr std::array<core::PartitionId, 1> Partitions() {
    return {kState};
  }
};

struct TpchQ3Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// ── Typed bulk-scan contract ────────────────────────────────────────────────
// One row per lineitem of customer ⨝ orders ⨝ lineitem. The rows of one order
// are consecutive (ordered by orderkey).
struct TpchQ3OrderLineInput {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
  std::string mktsegment;
  int32_t shipdate_days = 0;
  int64_t extended_price_raw = 0; // scale 2
  int64_t discount_raw = 0;       // scale 2
};

// The three tables of the join, scanned separately by the build (any order).
struct TpchQ3CustomerInput {
  int64_t custkey = 0;
  std::string mktsegment;
};

struct TpchQ3OrderInput {
  int64_t orderkey = 0;
  int64_t custkey = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
};

struct TpchQ3LineitemInput {
  int64_t orderkey = 0;
  int32_t shipdate_days = 0;
  int64_t extended_price_raw = 0; // scale 2
  int64_t discount_raw = 0;       // scale 2
};

class TpchQ3InitialScan {
public:
  virtual ~TpchQ3InitialScan() = default;
  virtual void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &) = 0;
  virtual void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &) = 0;
  // The join of the three, one row per lineitem, in orderkey order (the
  // fallback read streams it).
  virtual void ScanOrderLines(
      const std::function<void(const TpchQ3OrderLineInput &)> &) = 0;
  // The same rows for the given orders only, as the database holds them after
  // the calling statement.
  virtual bool LookupOrderLines(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const TpchQ3OrderLineInput &)> &) = 0;
  // Orders of the given customers (any customer row change may move them
  // between market segments).
  virtual bool LookupOrdersOfCustomers(const std::vector<int64_t> &custkeys,
                                       std::vector<int64_t> &orderkeys) = 0;
};

// ── State ───────────────────────────────────────────────────────────────────
// An order that has a customer and at least one lineitem is one record:
//   orderdate, shippriority and its lineitems as (shipdate, revenue) pairs,
//   revenue = SUM(extendedprice * (1 - discount)) at scale 4 per shipdate.
// Records of a market segment are grouped into bins by an upper bound of what
// the order can contribute to any answer (the sum of its positive per-shipdate
// revenues), so a top-k search walks the bins from the largest bound down and
// stops once the k-th best revenue exceeds the bound of the next bin.
//
// Memory: ~12 B per lineitem (shipdate + revenue) + ~40 B per order (record
// header, hash slot, locator).
//   SF100: ~11 GB   SF1000: ~110 GB   (before: ~75 B/order + a dense
//   order_segment array + a transient 45-50 GB node-map build at SF100)
// Write cost: an order write clones one bin shard (~2K records at SF1000) and
// one locator shard; the bin directory (~1K bins) is copied per batch.
struct TpchQ3State {
  static constexpr unsigned kBinShardBits = 9;
  using Bin = core::ShardedCowRecordStore<kBinShardBits>;

  struct Locator {
    uint32_t segment = 0;
    int32_t bin = 0;
  };

  // Market segments are data-driven: the dictionary grows with the data.
  std::map<std::string, uint32_t> segment_ids;
  // (segment id, bound bin) -> records of the orders in it, keyed by orderkey.
  std::map<std::pair<uint32_t, int32_t>, Bin> bins;
  // orderkey -> where its record is.
  core::ShardedCowHashMap<Locator, 16> locators;

  struct Bucket {
    int32_t shipdate = 0;
    int64_t revenue_raw = 0;
  };
  struct OrderRecord {
    int64_t orderkey = 0;
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
    std::vector<Bucket> buckets; // ascending shipdate, one per distinct day
  };

  // An order record ready to install: its bound bin and encoded bytes.
  struct PreparedOrder {
    int32_t bin = 0;
    std::vector<uint8_t> bytes;
  };
  static PreparedOrder Prepare(const OrderRecord &record);
  void Install(const std::string &mktsegment, int64_t orderkey,
               const PreparedOrder &order);

  // Replace the record of an order (no buckets: remove it).
  void SetOrder(const std::string &mktsegment, const OrderRecord &record);
  void RemoveOrder(int64_t orderkey);
};

struct TpchQ3StateImage {
  std::shared_ptr<const TpchQ3State> state;
};

struct TpchQ3OutputRow {
  int64_t orderkey = 0;
  int64_t revenue_raw = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
};

// The new state of one order, read from the database as the writing statement
// left it (the last one recorded for an order wins).
struct TpchQ3OrderResult {
  int64_t orderkey = 0;
  bool present = false; // has a customer and at least one lineitem
  std::string mktsegment;
  TpchQ3State::OrderRecord record;
};

// Largest revenue sum an order of bound-bin `bin` can have (see TpchQ3State).
int64_t TpchQ3BinUpperBound(int32_t bin);

// ── TxnBuffer ───────────────────────────────────────────────────────────────
class TpchQ3TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  void MarkRebuild();
  void AddOrderResult(TpchQ3OrderResult result);
  const std::vector<TpchQ3OrderResult> &OrderResults() const;

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ3OrderResult> order_results;
};

class TpchQ3Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ3Layout::kAccelerator;

  explicit TpchQ3Module(TpchQ3Config config,
                        TpchQ3InitialScan *initial_scan = nullptr)
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
  TpchQ3State BuildState(core::AuxEngineServices &) const;
  bool LoadState(core::AuxEngineServices &services,
                 std::shared_ptr<const TpchQ3State> &state,
                 core::Version *version = nullptr) const;
  void EmitResult(const std::vector<TpchQ3OutputRow> &, core::AuxRowSink &) const;

  TpchQ3Config config;
  TpchQ3InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
