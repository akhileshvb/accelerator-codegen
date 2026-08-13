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

struct TpchQ3Config {
  core::TableId customer_table = 0;
  core::TableId orders_table = 0;
  core::TableId lineitem_table = 0;
};

struct TpchQ3CustomerInput {
  int64_t key = 0;
  std::string mktsegment;
};

struct TpchQ3OrderInput {
  int64_t key = 0;
  int32_t orderdate_days = 0;
  int64_t custkey = 0;
  int32_t shippriority = 0;
};

struct TpchQ3LineitemInput {
  int64_t orderkey = 0;
  int64_t extended_price_raw = 0;
  int32_t extended_price_scale = 2;
  int64_t discount_raw = 0;
  int32_t discount_scale = 2;
  int32_t shipdate_days = 0;
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

  // orders ⨝ customer lookup for RF1 lineitems / orders Insert when the
  // dense order_segment side does not yet know orderkey.
  struct OrderContext {
    core::PartitionId segment = 0;
    int32_t orderdate_days = 0;
    int32_t shippriority = 0;
    int64_t custkey = 0;
  };
  virtual bool LookupOrderContext(int64_t orderkey, OrderContext &out) {
    (void)orderkey;
    (void)out;
    return false;
  }
};

struct TpchQ3ShipdateBucket {
  int32_t shipdate = 0;
  int64_t suffix_revenue = 0;
};

struct TpchQ3OrderSummary {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
  int32_t max_shipdate = 0;
  std::vector<TpchQ3ShipdateBucket> buckets;
};

struct TpchQ3SegmentIntervalIndex {
  core::PartitionId segment = 0;
  std::vector<TpchQ3OrderSummary> orders;
  std::unordered_map<int64_t, std::size_t> order_positions;
};

// Published segment-interval images hold only segment_intervals for one segment.
struct TpchQ3State {
  std::map<core::PartitionId, TpchQ3SegmentIntervalIndex> segment_intervals;
};

// Side partition: dense orderkey → segment id (partition id), 0 = absent.
struct TpchQ3OrderSegmentSide {
  std::vector<uint8_t> order_segment;
};

enum class TpchQ3ImageKind : uint8_t {
  OrderSegmentSide,
  SegmentIntervalIndex,
};

struct TpchQ3StateImage {
  TpchQ3ImageKind kind = TpchQ3ImageKind::OrderSegmentSide;
  core::PartitionId segment = 0;
  std::shared_ptr<const TpchQ3OrderSegmentSide> order_segment_side;
  std::shared_ptr<const TpchQ3State> state;
};

struct TpchQ3OutputRow {
  int64_t orderkey = 0;
  int64_t revenue_raw = 0;
  int32_t orderdate_days = 0;
  int32_t shippriority = 0;
};

struct TpchQ3LineitemDelta {
  core::PartitionId segment = 0;
  int64_t orderkey = 0;
  int32_t shipdate_day = 0;
  int64_t revenue_raw = 0;
  int8_t sign = 0;
};

struct TpchQ3OrderDelta {
  int64_t orderkey = 0;
  int32_t orderdate_days = 0;
  int64_t custkey = 0;
  int32_t shippriority = 0;
  int8_t sign = 0; // +1 insert, -1 delete
};

std::vector<uint8_t> EncodeTpchQ3OrderSegmentSide(
    const TpchQ3OrderSegmentSide &side);
std::vector<uint8_t> EncodeTpchQ3SegmentIntervalIndex(
    const TpchQ3SegmentIntervalIndex &);
bool DecodeTpchQ3OrderSegmentSide(const std::vector<uint8_t> &,
                                    TpchQ3OrderSegmentSide &);
bool DecodeTpchQ3SegmentIntervalIndex(const std::vector<uint8_t> &,
                                        core::PartitionId segment,
                                        TpchQ3SegmentIntervalIndex &);

int64_t TpchQ3RevenueRaw(int64_t extended_price_raw, int32_t extended_scale,
                         int64_t discount_raw, int32_t discount_scale);
core::PartitionId TpchQ3PartitionFromMktsegment(const std::string &segment);
std::string TpchQ3MktsegmentFromPartition(core::PartitionId partition);
int32_t TpchQ3DaysFromYmd(int32_t year, unsigned month, unsigned day);

void FinalizeTpchQ3OrderSummary(TpchQ3OrderSummary &,
                                const std::map<int32_t, int64_t> &day_revenue);
int64_t TpchQ3RevenueAboveCutoff(const TpchQ3OrderSummary &, int32_t cutoff_days);
void SortTpchQ3SegmentIntervalIndex(TpchQ3SegmentIntervalIndex &);

std::vector<TpchQ3OutputRow>
ComposeTpchQ3TopK(const TpchQ3SegmentIntervalIndex &, int32_t cutoff_days);

class TpchQ3TxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override;
  void Clear() override;

  bool RequiresRebuild() const;
  const std::vector<TpchQ3LineitemDelta> &Deltas() const;
  const std::vector<TpchQ3OrderDelta> &OrderDeltas() const;
  void MarkRebuild();
  void AddDelta(TpchQ3LineitemDelta delta);
  void AddOrderDelta(TpchQ3OrderDelta delta);

private:
  bool pending = false;
  bool requires_rebuild = false;
  std::vector<TpchQ3LineitemDelta> deltas;
  std::vector<TpchQ3OrderDelta> order_deltas;
};

class TpchQ3Module final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::TpchQ3Layout::kAccelerator;

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

private:
  struct BuildScratch {
    std::unordered_map<int64_t, core::PartitionId> customer_segments;
    std::vector<uint8_t> order_segment;
  };

  TpchQ3State BuildState(core::AuxEngineServices &, BuildScratch &) const;
  bool LoadSegmentIntervalIndex(core::AuxEngineServices &, core::PartitionId,
                                const TpchQ3SegmentIntervalIndex *&) const;
  void EmitResult(const std::vector<TpchQ3OutputRow> &, core::AuxRowSink &) const;

  TpchQ3Config config;
  TpchQ3InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
