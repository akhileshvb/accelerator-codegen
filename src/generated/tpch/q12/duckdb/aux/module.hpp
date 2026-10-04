#pragma once

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
#include "accelerator_codegen/stateful/core/dense_id_array.hpp"
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

// Copy-on-write paged dense array indexed by order key. Copying the struct
// copies only the page table (shared_ptr per 64K keys); Set() clones a page
// only while it is still shared with another (published) copy. A published
// copy is never mutated: writers always work on their own copy.
template <typename T> struct TpchQ12PagedArray {
  static constexpr std::size_t kShift = 16;
  static constexpr std::size_t kSize = std::size_t(1) << kShift;
  static constexpr std::size_t kMask = kSize - 1;
  static constexpr std::size_t kMaxPages = 4096;
  static constexpr int64_t kMaxId = static_cast<int64_t>(kMaxPages) << kShift;

  std::vector<std::shared_ptr<std::vector<T>>> pages;

  static bool Fits(int64_t key) { return key >= 0 && key < kMaxId; }
  void Clear() { pages.clear(); }

  T Get(int64_t key) const {
    if (!Fits(key))
      return T{};
    const auto id = static_cast<std::size_t>(key);
    const auto page = id >> kShift;
    if (page >= pages.size() || !pages[page])
      return T{};
    return (*pages[page])[id & kMask];
  }

  bool Set(int64_t key, T value) {
    if (!Fits(key))
      return false;
    const auto id = static_cast<std::size_t>(key);
    const auto page = id >> kShift;
    if (pages.size() <= page)
      pages.resize(page + 1);
    auto &ptr = pages[page];
    if (!ptr)
      ptr = std::make_shared<std::vector<T>>(kSize, T{});
    else if (ptr.use_count() > 1)
      ptr = std::make_shared<std::vector<T>>(*ptr);
    (*ptr)[id & kMask] = value;
    return true;
  }
};

struct TpchQ12State {
  TpchQ12PagedArray<uint8_t> order_priorities;
  // Number of qualifying lineitems counted for each present order. An order
  // can be removed without moving contributions only when this is zero.
  TpchQ12PagedArray<uint16_t> line_counts;
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

enum class TpchQ12ImageKind : uint8_t { OrderPriorities, Day };

struct TpchQ12StateImage {
  TpchQ12ImageKind kind = TpchQ12ImageKind::OrderPriorities;
  int32_t receipt_day = 0;
  std::shared_ptr<const TpchQ12State> state;
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

// `days` holds the per-receipt-day shipmode maps in the query window (null
// entries are skipped).
void TpchQ12FinalizeAndEmit(const std::vector<const TpchQ12DayMap *> &days,
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
