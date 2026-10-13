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
#include "accelerator_codegen/stateful/prims/sharded_sorted_map.hpp"

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

// Order eligibility index: a persistent (copy-on-write) orderkey -> order
// info map (core::ShardedSortedMap, 2^16 shards = 256 x 256, key-sorted
// leaves).  A published index is never mutated; a write copies the top array,
// the touched mid arrays and the touched leaves (a few hundred entries each).
using TpchQ4EligIndex =
    core::ShardedSortedKvMap<int64_t, TpchQ4OrderInfo,
                             core::ShardedSortedMapHash<int64_t>, 16, 8>;

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


// Published tree: 2 * tree_size nodes of tree_stride counts each, held in
// immutable pages of kTpchQ4PageNodes nodes so a write copies only the pages
// on the touched leaf-to-root paths.
constexpr uint32_t kTpchQ4PageNodes = 64;
struct TpchQ4TreePayload {
  uint32_t tree_size = 0;
  uint32_t tree_stride = 0;
  std::vector<std::shared_ptr<const std::vector<int64_t>>> pages;
  bool WellFormed() const {
    const std::size_t nodes = static_cast<std::size_t>(tree_size) * 2;
    return tree_stride > 0 &&
           pages.size() == (nodes + kTpchQ4PageNodes - 1) / kTpchQ4PageNodes;
  }
  bool Valid(uint32_t node) const {
    return node < static_cast<std::size_t>(tree_size) * 2 &&
           node / kTpchQ4PageNodes < pages.size();
  }
  const int64_t *Node(uint32_t node) const {
    return pages[node / kTpchQ4PageNodes]->data() +
           static_cast<std::size_t>(node % kTpchQ4PageNodes) * tree_stride;
  }
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
