#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"
#include "accelerator_codegen/stateful/prims/any_key_cow_array.hpp"
#include "accelerator_codegen/stateful/prims/dense_id_array.hpp"
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::query {

struct TpchQ12Config {
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ12OrderInput {
  int64_t key = 0;
  std::string orderpriority;
};

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
  // Qualifying lineitems (l_commitdate < l_receiptdate AND l_shipdate <
  // l_commitdate) of the given orders in the current base-table snapshot
  // (including this transaction's own writes). False: unsupported.
  virtual bool LookupQualifyingLineitems(
      const std::vector<int64_t> &,
      const std::function<void(int64_t orderkey, int32_t receipt_day,
                               const std::string &shipmode)> &) {
    return false;
  }
  virtual void ScanLineitemsForReceiptDays(
      const std::set<int32_t> &receipt_days,
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) {
    if (receipt_days.empty())
      return;
    ScanLineitems([&](const TpchQ12LineitemInput &input) {
      if (receipt_days.count(input.receiptdate_days))
        consumer(input);
    });
  }
};

enum class TpchQ12PriorityClass : uint8_t {
  Absent = 0,
  High = 1,
  Low = 2,
  Neither = 3,
};

struct TpchQ12DayShipmodeStats {
  int64_t high_line_count = 0;
  int64_t low_line_count = 0;
};

struct TpchQ12State {
  // Per-orderkey columns; a published copy is never mutated (writers work on
  // their own copy, which shares untouched chunks).
  core::AnyKeyCowArray<uint8_t, 12> order_priorities{};
  // Number of qualifying lineitems counted for each present order. An order
  // can be removed without moving contributions only when this is zero.
  core::AnyKeyCowArray<uint16_t, 12> line_counts{};
  std::map<int32_t, std::unordered_map<std::string, TpchQ12DayShipmodeStats>>
      day_partitions;

  TpchQ12PriorityClass PriorityOf(int64_t orderkey) const {
    return static_cast<TpchQ12PriorityClass>(order_priorities.Get(orderkey));
  }
  void SetPriority(int64_t orderkey, TpchQ12PriorityClass priority) {
    order_priorities.Set(orderkey, static_cast<uint8_t>(priority));
  }
};
using TpchQ12DayMap = std::unordered_map<std::string, TpchQ12DayShipmodeStats>;

// The seven TPC-H ship modes get a fixed slot (dense, branch-free adds); any
// other name lives in `extra` (empty for TPC-H data). Zero means absent.
constexpr int kQ12KnownModes = 7;
constexpr int kQ12ModeSlots = 8;

// Slot of a known ship mode, or -1.
int TpchQ12ModeSlot(const std::string &shipmode);

struct TpchQ12ModeStats {
  std::string shipmode;
  int64_t high_line_count = 0;
  int64_t low_line_count = 0;
};

struct alignas(64) TpchQ12Modes {
  int64_t high[kQ12ModeSlots] = {};
  int64_t low[kQ12ModeSlots] = {};
  std::vector<TpchQ12ModeStats> extra;

  // Adds (high, low) for `shipmode`; slot < 0 means "look up / add in extra".
  void Add(int slot, const std::string &shipmode, int64_t h, int64_t l);
  bool Empty() const;
};

// One partition of the dyadic tree over receipt days. Tree nodes (heap
// numbering) use `modes`; the single overflow partition (days outside the tree
// domain) uses `overflow_days`.
struct TpchQ12Node {
  TpchQ12Modes modes;
  std::map<int32_t, TpchQ12Modes> overflow_days;
};

enum class TpchQ12ImageKind : uint8_t { OrderPriorities, Node };

struct TpchQ12StateImage {
  TpchQ12ImageKind kind = TpchQ12ImageKind::OrderPriorities;
  int32_t receipt_day = 0;
  std::shared_ptr<const TpchQ12State> state;
  TpchQ12Node node; // the node lives in the image: one allocation per node
};

struct TpchQ12LineitemDelta {
  int64_t orderkey = 0;
  int32_t receipt_day = 0;
  std::string shipmode;
  TpchQ12PriorityClass priority = TpchQ12PriorityClass::Absent;
  int8_t sign = 0;
};

struct TpchQ12OrderDelta {
  int64_t orderkey = 0;
  TpchQ12PriorityClass priority = TpchQ12PriorityClass::Absent;
  int8_t sign = 0; // +1 insert, -1 delete
};

std::vector<uint8_t> EncodeTpchQ12OrderPriorities(const TpchQ12State &);
std::vector<uint8_t> EncodeTpchQ12Day(int32_t receipt_day, const TpchQ12State &);
bool DecodeTpchQ12OrderPriorities(const std::vector<uint8_t> &, TpchQ12State &);
bool DecodeTpchQ12Day(const std::vector<uint8_t> &, int32_t receipt_day,
                      TpchQ12State &);
bool FindTpchQ12OrderPriority(const std::vector<uint8_t> &, int64_t orderkey,
                              TpchQ12PriorityClass &);

TpchQ12PriorityClass TpchQ12ClassifyPriority(const std::string &priority);
bool TpchQ12LineitemQualifies(int32_t commitdate_days, int32_t receiptdate_days,
                              int32_t shipdate_days);
int32_t TpchQ12DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ12AddMonths(int32_t days_since_epoch, int32_t months);

// `parts` holds the aligned tree nodes covering the query window (null entries
// are skipped).
void TpchQ12FinalizeAndEmit(const TpchQ12Modes *const *parts,
                            std::size_t part_count,
                            const std::string &shipmode1,
                            const std::string &shipmode2, core::AuxRowSink &sink);

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

  TpchQ12Config config;
  TpchQ12InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
