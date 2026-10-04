#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_query_module.hpp"

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

struct TpchQ4OrderInput {
  int64_t key = 0;
  int32_t orderdate_days = 0;
  std::string orderpriority;
};

struct TpchQ4LineitemInput {
  int64_t orderkey = 0;
  int32_t commitdate_days = 0;
  int32_t receiptdate_days = 0;
};

class TpchQ4InitialScan {
public:
  virtual ~TpchQ4InitialScan() = default;
  virtual void ScanOrders(
      const std::function<void(const TpchQ4OrderInput &)> &) = 0;
  virtual void ScanLineitems(
      const std::function<void(const TpchQ4LineitemInput &)> &) = 0;
};

struct TpchQ4OrderInfo {
  int32_t orderdate_days = 0;
  int32_t priority_idx = 0;
  int64_t qualifying_count = 0;
};

// Order eligibility index: a two-level (256 x 256) sharded table of immutable,
// key-sorted leaves.  A published index is never mutated; a write copies the
// top array (256 shared_ptrs), the touched mid arrays (256 shared_ptrs each)
// and the touched leaves (a few hundred entries each) -- see
// TpchQ4EligTxn.  A null pointer denotes an empty mid/leaf.
struct TpchQ4EligEntry {
  int64_t key = 0;
  TpchQ4OrderInfo info;
};
using TpchQ4EligLeaf = std::vector<TpchQ4EligEntry>;
struct TpchQ4EligMid {
  std::array<std::shared_ptr<const TpchQ4EligLeaf>, 256> leaves;
};
struct TpchQ4EligIndex {
  std::array<std::shared_ptr<const TpchQ4EligMid>, 256> mids;
  const TpchQ4OrderInfo *Find(int64_t key) const;
};

// Tree counts are stored flat: node n, priority p at tree[n * tree_stride + p]
// (tree has 2 * tree_size nodes).
struct TpchQ4State {
  std::vector<std::string> priority_names;
  std::unordered_map<std::string, int32_t> priority_map;
  TpchQ4EligIndex order_eligibility;
  int32_t min_orderdate_days = 0;
  int32_t max_orderdate_days = 0;
  uint32_t tree_size = 0;
  uint32_t tree_stride = 0;
  std::vector<int64_t> tree;
};


struct TpchQ4TreePayload {
  uint32_t tree_size = 0;
  uint32_t tree_stride = 0;
  std::vector<int64_t> tree;
};

struct TpchQ4EligibilityPayload {
  TpchQ4EligIndex order_eligibility;
};

enum class TpchQ4ImageKind : uint8_t {
  Side,
  Tree,
  Eligibility,
};

struct TpchQ4StateImage {
  TpchQ4ImageKind kind = TpchQ4ImageKind::Side;
  std::shared_ptr<const TpchQ4State> state;
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
  int8_t sign = 0;
};

int32_t TpchQ4DaysFromYmd(int32_t year, unsigned month, unsigned day);
int32_t TpchQ4AddMonths(int32_t days_since_epoch, int32_t months);
int32_t TpchQ4PriorityIndex(const std::string &priority,
                            std::vector<std::string> &names,
                            std::unordered_map<std::string, int32_t> &pmap);
void FinalizeTpchQ4(const std::vector<int64_t> &counts,
                    const std::vector<std::string> &priority_names,
                    core::AuxRowSink &sink);

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
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;
  std::vector<core::CalibrationWrite> CalibrationWrites() const override;

private:
  TpchQ4State BuildState(core::AuxEngineServices &) const;
  bool LoadSide(core::AuxEngineServices &, TpchQ4State &,
                std::shared_ptr<const TpchQ4EligibilityPayload> &) const;
  bool LoadMeta(core::AuxEngineServices &, TpchQ4State &) const;
  bool LoadTree(core::AuxEngineServices &, const TpchQ4State &,
                const std::vector<uint32_t> &node_indices,
                std::vector<int64_t> &counts) const;
  void EmitResult(const TpchQ4State &, const std::vector<int64_t> &,
                  core::AuxRowSink &) const;

  TpchQ4Config config;
  TpchQ4InitialScan *initial_scan = nullptr;
};

}  // namespace accelerator_codegen::aux_state::query
