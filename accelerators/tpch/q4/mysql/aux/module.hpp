#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "substrate/aux/core/aux_query_module.hpp"
#include "prims/radix_range_tree.hpp"
#include "prims/any_key_cow_array.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ4Layout {
  static constexpr const char *kAccelerator = "tpch_q4";
  static constexpr core::PartitionId kSide = 0;
  static constexpr core::PartitionId kTreeBase = 1;
  static constexpr core::PartitionId kEligibility = 2;
};

struct TpchQ4Config {
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

// Orders with a NULL orderdate / orderkey never qualify and are not delivered.
struct TpchQ4OrderInput {
  int64_t key = 0;
  int32_t orderdate_days = 0;
  std::string orderpriority;
  bool priority_is_null = false;
};

struct TpchQ4LineitemInput {
  int64_t orderkey = 0;
  int32_t commitdate_days = 0;
  int32_t receiptdate_days = 0;
};

class TpchQ4InitialScan {
public:
  virtual ~TpchQ4InitialScan() = default;
  // Split into `partitions` fixed row ranges: sink(partition, row) runs
  // concurrently for different partitions, one thread per partition.
  virtual void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ4OrderInput &)> &sink) = 0;
  virtual void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ4LineitemInput &)> &sink) = 0;
};

// ---------------------------------------------------------------------------
// State layout (all per-write copies are O(touched chunks), never O(state)).
//
//   kEligibility  per-order columns keyed by o_orderkey (AnyKeyCowArray):
//                 orderdate (4 B), priority index (2 B), count of lineitems
//                 with commitdate < receiptdate (2 B)  => 8 B per key slot.
//                 TPC-H keys are ~25% dense, so ~32 B/order incl. the gaps
//                 (dense keys: 8 B/order).  SF100 (150M orders): ~4.8 GB,
//                 SF1000: ~48 GB; the former unordered_map was ~56 B/order
//                 (8.4 GB at SF100) plus a full map copy on every write.
//                 Keys outside [0, 2^36) live in a sharded COW hash map
//                 (core/any_key_cow_array.hpp).
//   kTreeBase     per priority, a radix-64 counting tree over the order date
//                 (core::RadixRangeTree: 4 levels x 6 bits = 2^24 day slots
//                 around the epoch):
//                 count of eligible orders per (priority, day).  A date range
//                 query reads <= 2*63 cells per level (O(log D)) per priority;
//                 an order insert/delete/eligibility flip touches 4 cells.
//                 ~130 KB per priority regardless of SF.  Dates outside the
//                 tree window (|days| > ~23000 years) go to a small ordered
//                 outlier map in the side image.
//   kSide         priority dictionary (+ NULL priority slot) and outliers.
// ---------------------------------------------------------------------------

constexpr uint32_t kTpchQ4NoOrder = 0xFFFFFFFFu;  // priority sentinel: no order row

struct TpchQ4OrderInfo {
  int32_t orderdate_days = 0;
  uint32_t priority_idx = kTpchQ4NoOrder;  // kTpchQ4NoOrder: no order row
  uint32_t qualifying_count = 0;           // lineitems with commit < receipt
  bool HasOrder() const { return priority_idx != kTpchQ4NoOrder; }
  bool Eligible() const { return HasOrder() && qualifying_count > 0; }
};

// Order columns keyed by orderkey (any int64).  Slots never written read as
// "no order, no lineitems".  Stored as 4 + 2 + 2 bytes per key slot: the
// priority index in 16 bits (kTpchQ4NoOrder = no order row; at most 65534
// distinct priorities, more throws) and the lineitem count in 16 bits, with
// counts >= 0xFFFF kept exactly in a small sharded hash map.
class TpchQ4OrderTable {
public:
  TpchQ4OrderTable() : orderdate_(0), priority_(kNoOrder16), qualifying_(0) {}

  TpchQ4OrderInfo Get(int64_t key) const {
    TpchQ4OrderInfo info;
    info.orderdate_days = orderdate_.Get(key);
    const uint16_t p = priority_.Get(key);
    info.priority_idx = p == kNoOrder16 ? kTpchQ4NoOrder : p;
    const uint16_t q = qualifying_.Get(key);
    if (q != kBigCount) {
      info.qualifying_count = q;
    } else {
      const uint32_t *big = big_counts_.Find(key);
      info.qualifying_count = big ? *big : q;
    }
    return info;
  }
  // Writes only the columns that change (an unchanged chunk is never cloned).
  void Set(int64_t key, const TpchQ4OrderInfo &info) {
    orderdate_.Set(key, info.orderdate_days);
    priority_.Set(key, EncodePriority(info.priority_idx));
    SetCount(key, info.qualifying_count);
  }
  // Build fast path: one more qualifying lineitem for `key`.
  void AddQualifying(int64_t key) {
    const uint16_t q = qualifying_.Get(key);
    if (q < kBigCount - 1)
      qualifying_.Set(key, static_cast<uint16_t>(q + 1));
    else
      SetCount(key, Get(key).qualifying_count + 1);
  }
  // Build bucket of `key`. The keys of one 2^16-id chunk (of every column)
  // share a bucket, so tables built per bucket hold disjoint chunks.
  static std::size_t BucketOf(int64_t key, std::size_t buckets) {
    using Ids = core::AnyKeyCowArray<uint16_t, 16>;
    const uint64_t id = Ids::Zig(key);
    return (Ids::IsDense(key) ? id >> 16 : id) % buckets;
  }
  // Folds in a table over a disjoint key set (another build bucket).
  void MergeDisjoint(TpchQ4OrderTable &&other) {
    const auto theirs = [](auto, auto value) { return value; };
    orderdate_.MergeFrom(std::move(other.orderdate_), theirs);
    priority_.MergeFrom(std::move(other.priority_), theirs);
    qualifying_.MergeFrom(std::move(other.qualifying_), theirs);
    other.big_counts_.ForEach(
        [&](int64_t key, uint32_t count) { big_counts_.Upsert(key, count); });
  }
  std::size_t MemoryBytes() const {
    return orderdate_.MemoryBytes() + priority_.MemoryBytes() +
           qualifying_.MemoryBytes() + big_counts_.memory_bytes();
  }

private:
  static constexpr uint16_t kNoOrder16 = 0xFFFF;
  static constexpr uint16_t kBigCount = 0xFFFF;

  static uint16_t EncodePriority(uint32_t idx) {
    if (idx == kTpchQ4NoOrder)
      return kNoOrder16;
    if (idx >= kNoOrder16)
      throw std::length_error("TPC-H Q4: more than 65534 distinct priorities");
    return static_cast<uint16_t>(idx);
  }
  void SetCount(int64_t key, uint32_t count) {
    if (count < kBigCount) {
      qualifying_.Set(key, static_cast<uint16_t>(count));
      if (big_counts_.Find(key))
        big_counts_.Erase(key);
    } else {
      qualifying_.Set(key, kBigCount);
      big_counts_.Upsert(key, count);
    }
  }

  core::AnyKeyCowArray<int32_t, 15> orderdate_;
  core::AnyKeyCowArray<uint16_t, 16> priority_;
  core::AnyKeyCowArray<uint16_t, 16> qualifying_;
  core::ShardedCowHashMap<uint32_t, 6> big_counts_;
};

// Counting tree over order-date days for ONE priority (core::RadixRangeTree):
// the number of eligible orders per day.  Day key = days + kDayBias, valid in
// [0, 2^24).
class TpchQ4DayTree {
public:
  static constexpr int64_t kDayBias = int64_t{1} << 23;
  static constexpr int64_t kKeySpan = core::RadixRangeTree<int64_t>::kKeySpan;

  static bool InWindow(int64_t days) {
    return core::RadixRangeTree<int64_t>::InRange(days + kDayBias);
  }
  void Add(int64_t days, int64_t by) { tree_.Add(days + kDayBias, by); }
  // Sum over day keys [a, b).
  int64_t SumKeys(int64_t a, int64_t b) const { return tree_.SumRange(a, b); }
  std::size_t MemoryBytes() const { return tree_.MemoryBytes(); }

private:
  core::RadixRangeTree<int64_t> tree_;
};

// kSide: dictionary + outlier dates.
struct TpchQ4Side {
  std::vector<std::string> priority_names;
  std::unordered_map<std::string, int32_t> priority_map;
  int32_t null_priority_idx = -1;  // index of the NULL priority group, if any
  // (day, priority) -> eligible orders, for days outside the tree window.
  std::map<std::pair<int64_t, uint32_t>, int64_t> outliers;
};

struct TpchQ4TreePayload {
  std::vector<TpchQ4DayTree> by_priority;  // indexed by priority idx
};

struct TpchQ4EligibilityPayload {
  TpchQ4OrderTable orders;
};

// Everything, as assembled by a build (and by the exact fallback).
struct TpchQ4State {
  TpchQ4Side side;
  TpchQ4TreePayload tree;
  TpchQ4EligibilityPayload eligibility;
};

enum class TpchQ4ImageKind : uint8_t {
  Side,
  Tree,
  Eligibility,
};

struct TpchQ4StateImage {
  TpchQ4ImageKind kind = TpchQ4ImageKind::Side;
  std::shared_ptr<const TpchQ4Side> state;
};

struct TpchQ4TreeImage {
  TpchQ4ImageKind kind = TpchQ4ImageKind::Tree;
  std::shared_ptr<const TpchQ4TreePayload> payload;
};

struct TpchQ4EligibilityImage {
  TpchQ4ImageKind kind = TpchQ4ImageKind::Eligibility;
  std::shared_ptr<const TpchQ4EligibilityPayload> payload;
};

struct TpchQ4LineitemDelta {
  int64_t orderkey = 0;
  int8_t sign = 0;
};

struct TpchQ4OrderDelta {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  std::string orderpriority;
  bool priority_is_null = false;
  int8_t sign = 0;
};

uint32_t TpchQ4PriorityIndex(const std::string &priority, bool is_null,
                             TpchQ4Side &side);
void FinalizeTpchQ4(const std::vector<int64_t> &counts,
                    const TpchQ4Side &side, core::AuxRowSink &sink);

class TpchQ4TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ4LineitemDelta> &Deltas() const;
  const std::vector<TpchQ4OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ4LineitemDelta delta);
  void AddOrderDelta(TpchQ4OrderDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ4LineitemDelta> deltas;
  std::vector<TpchQ4OrderDelta> order_deltas;
};

class TpchQ4Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = TpchQ4Layout::kAccelerator;

  explicit TpchQ4Module(TpchQ4Config config,
                        TpchQ4InitialScan *initial_scan = nullptr)
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
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ4State BuildState(core::AuxEngineServices &) const;
  // Per-priority counts of eligible orders with date in [start, end).
  static std::vector<int64_t> CountRange(const TpchQ4Side &,
                                         const TpchQ4TreePayload &,
                                         int64_t start, int64_t end);

  TpchQ4Config config;
  TpchQ4InitialScan *initial_scan = nullptr;
};

}  // namespace accelerator_codegen::aux_state::query
