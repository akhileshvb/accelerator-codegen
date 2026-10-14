#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/flat_hash_map.hpp"
#include "prims/any_key_cow_array.hpp"
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::query {

// ---------------------------------------------------------------------------
// State layout (published as three images; a write batch copies directories /
// shard pointers and clones only the chunks and shards it touches).
//
//   kOrderPriorities  priority class per orderkey: AnyKeyCowArray<uint8_t>,
//                     1 B per key slot (TPC-H keys are ~25% dense => ~4 B per
//                     order: 0.6 GB at SF100, 6 GB at SF1000; the old
//                     DenseIdArray + whole-array copy per write batch was the
//                     same size but copied up to 268 MB per batch).
//   kCells            per (receipt day, shipmode): lineitem counts joined with
//                     an existing order, split high / low / total.  Only
//                     qualifying lineitems (commit < receipt, ship < commit)
//                     are counted: ~2.5K days x 7 modes = ~20K cells (~1 MB),
//                     independent of SF.
//   kSide             shipmode dictionary.
// Execute reads 2 x 366 cells: O(1) in the data size.
// ---------------------------------------------------------------------------
struct TpchQ12Parts {
  static constexpr core::PartitionId kOrderPriorities =
      core::TpchQ12Layout::kOrderPriorities;
  static constexpr core::PartitionId kCells = 1;
  static constexpr core::PartitionId kSide = 2;
};

struct TpchQ12Config {
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// An order with a NULL o_orderkey can never join and is not delivered.
struct TpchQ12OrderInput {
  int64_t key = 0;
  std::string orderpriority;
  bool priority_is_null = false;
};

// Only qualifying lineitems are delivered by the scans (non-NULL orderkey and
// shipmode, commit < receipt, ship < commit); the module re-checks anyway.
struct TpchQ12LineitemInput {
  int64_t orderkey = 0;
  std::string shipmode;
  int32_t commitdate_days = 0;
  int32_t receiptdate_days = 0;
  int32_t shipdate_days = 0;
};

class TpchQ12InitialScan {
public:
  virtual ~TpchQ12InitialScan() = default;
  virtual void ScanOrders(
      const std::function<void(const TpchQ12OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ12LineitemInput &)> &) = 0;
  // Qualifying lineitems of the given orders, as the database stands now
  // (including this transaction's writes).  Backends override this with an
  // index lookup; the default filters a full scan.
  virtual void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) {
    if (orderkeys.empty())
      return;
    const std::set<int64_t> wanted(orderkeys.begin(), orderkeys.end());
    ScanLineitems([&](const TpchQ12LineitemInput &input) {
      if (wanted.count(input.orderkey))
        consumer(input);
    });
  }
};

enum class TpchQ12PriorityClass : uint8_t {
  Absent = 0,   // no order row for the key
  High = 1,     // '1-URGENT' / '2-HIGH'
  Low = 2,      // any other non-NULL priority
  Neither = 3,  // NULL priority: joins, but is neither high nor low
};

struct TpchQ12Counts {
  int64_t high = 0;
  int64_t low = 0;
  int64_t rows = 0;  // joined qualifying lineitems (also those counted in neither)

  bool operator==(const TpchQ12Counts &o) const {
    return high == o.high && low == o.low && rows == o.rows;
  }
  TpchQ12Counts &operator+=(const TpchQ12Counts &o) {
    high += o.high;
    low += o.low;
    rows += o.rows;
    return *this;
  }
};

struct TpchQ12State {
  core::AnyKeyCowArray<uint8_t, 16> order_class;
  // key = receipt_day * 2^24 + shipmode id.
  core::ShardedCowHashMap<TpchQ12Counts, 6> cells;
  std::vector<std::string> shipmodes;
  std::unordered_map<std::string, uint32_t> shipmode_ids;

  TpchQ12PriorityClass PriorityOf(int64_t orderkey) const {
    return static_cast<TpchQ12PriorityClass>(order_class.Get(orderkey));
  }
  void SetPriority(int64_t orderkey, TpchQ12PriorityClass priority) {
    order_class.Set(orderkey, static_cast<uint8_t>(priority));
  }
};

enum class TpchQ12ImageKind : uint8_t { OrderPriorities, Cells, Side };

struct TpchQ12StateImage {
  TpchQ12ImageKind kind = TpchQ12ImageKind::OrderPriorities;
  std::shared_ptr<const TpchQ12State> state;
};

struct TpchQ12LineitemDelta {
  int64_t orderkey = 0;
  int32_t receipt_day = 0;
  std::string shipmode;
  int8_t sign = 0;
};

struct TpchQ12OrderDelta {
  int64_t orderkey = 0;
  TpchQ12PriorityClass priority = TpchQ12PriorityClass::Absent;
  int8_t sign = 0; // +1 insert, -1 delete
};

TpchQ12PriorityClass TpchQ12ClassifyPriority(const std::string &priority,
                                             bool is_null = false);
bool TpchQ12LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days,
                              int32_t shipdate_days);
int32_t TpchQ12DaysFromYmd(int32_t year, unsigned month, unsigned day);
int64_t TpchQ12AddMonths(int64_t days_since_epoch, int64_t months);

// One result group: a shipmode with at least one joined qualifying lineitem.
struct TpchQ12Group {
  std::string shipmode;
  TpchQ12Counts counts;
};
void TpchQ12FinalizeAndEmit(std::vector<TpchQ12Group> groups,
                            core::AuxRowSink &sink);

class TpchQ12TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ12LineitemDelta> &Deltas() const;
  const std::vector<TpchQ12OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ12LineitemDelta delta);
  void AddOrderDelta(TpchQ12OrderDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ12LineitemDelta> deltas;
  std::vector<TpchQ12OrderDelta> order_deltas;
};

class TpchQ12Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ12Layout::kAccelerator;

  explicit TpchQ12Module(TpchQ12Config config,
                         TpchQ12InitialScan *initial_scan = nullptr)
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
  TpchQ12State BuildState(core::AuxEngineServices &) const;
  // Groups for shipmode IN (mode1, mode2) and receipt day in [start, end).
  static std::vector<TpchQ12Group>
  Answer(const TpchQ12State &cells_and_side, const core::QueryArguments &);

  TpchQ12Config config;
  TpchQ12InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
