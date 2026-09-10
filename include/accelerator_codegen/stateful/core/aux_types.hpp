// SCAFFOLDING
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "accelerator_codegen/stateful/state/invalidation_plan.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::core {

using AcceleratorId = std::string;
using TableId = uint64_t;
using ColumnId = uint32_t;
using PartitionId = uint64_t;
using Version = uint64_t;

enum class MutationKind : uint8_t {
  Insert,
  Delete,
  Update,
};

enum class AuxType : uint8_t {
  Boolean,
  Int64,
  UInt64,
  ScaledDecimal,
  Double,
  Date,
  String,
};

struct ScaledDecimal {
  int64_t value = 0;
  int32_t scale = 0;

  bool operator==(const ScaledDecimal &other) const {
    return value == other.value && scale == other.scale;
  }
};

struct AuxDate {
  int32_t days_since_epoch = 0;

  bool operator==(const AuxDate &other) const {
    return days_since_epoch == other.days_since_epoch;
  }
};

using AuxPayload = std::variant<bool, int64_t, uint64_t, ScaledDecimal, double,
                                AuxDate, std::string>;

struct AuxValue {
  AuxType type = AuxType::String;
  bool is_null = true;
  AuxPayload payload = std::string();

  static AuxValue Null(AuxType type_p) {
    AuxValue value;
    value.type = type_p;
    value.is_null = true;
    return value;
  }

  template <typename T> static AuxValue Of(AuxType type_p, T payload_p) {
    AuxValue value;
    value.type = type_p;
    value.is_null = false;
    value.payload = std::move(payload_p);
    return value;
  }
};

struct AuxRow {
  std::vector<AuxValue> values;
};

struct MutationBatch {
  TableId table = 0;
  MutationKind kind = MutationKind::Insert;
  std::vector<AuxRow> old_rows;
  std::vector<AuxRow> new_rows;
  std::vector<ColumnId> updated_columns;
};

struct StateImage {
  AcceleratorId accelerator;
  PartitionId partition = 0;
  Version version = 0;

  // State is query-owned.  In-process engines keep the immutable typed
  // object here; an engine that has to cross a process boundary can encode it
  // at its own adapter boundary.  The common layer never has to deserialize
  // an image merely to execute a query.
  std::shared_ptr<const void> payload;

  template <typename T>
  static StateImage Make(AcceleratorId accelerator_p, PartitionId partition_p,
                         Version version_p,
                         std::shared_ptr<const T> payload_p) {
    StateImage image;
    image.accelerator = std::move(accelerator_p);
    image.partition = partition_p;
    image.version = version_p;
    image.payload = std::move(payload_p);
    return image;
  }

  template <typename T> const T *As() const {
    return static_cast<const T *>(payload.get());
  }
};

struct VisibilityToken {
  Version mutation_epoch = 0;
  uint64_t transaction_id = 0;
  std::string backend_token;

  bool operator==(const VisibilityToken &other) const {
    return mutation_epoch == other.mutation_epoch &&
           transaction_id == other.transaction_id &&
           backend_token == other.backend_token;
  }
};

struct StateSnapshot {
  AcceleratorId accelerator;
  VisibilityToken visibility;
  std::vector<StateImage> partitions;
  bool available = false;
};

enum class ValidityCode : uint8_t {
  Valid,
  Missing,
  Invalidated,
  NotVisible,
  Conflict,
};

struct ValidityResult {
  ValidityCode code = ValidityCode::Missing;
  Version visible_version = 0;

  bool IsValid() const { return code == ValidityCode::Valid; }
};

struct QueryArguments {
  std::vector<AuxValue> values;
};

struct OutputColumn {
  std::string name;
  AuxType type = AuxType::String;
  bool nullable = true;
};

struct OutputSchema {
  std::vector<OutputColumn> columns;
};

struct QueryKey {
  AcceleratorId accelerator;
  std::string value;

  bool operator==(const QueryKey &other) const {
    return accelerator == other.accelerator && value == other.value;
  }
};

struct TableDependency {
  TableId table = 0;
  std::vector<ColumnId> watched_columns;
};

using InvalidationPlan = ::accelerator_codegen::aux_state::InvalidationPlan;
using MaintenanceAction =
    ::accelerator_codegen::aux_state::maintenance::MaintenanceAction;

struct StagedMaintenanceOutcome {
  MaintenanceAction action = MaintenanceAction::NoOp;
  std::vector<StateImage> updates;
  InvalidationPlan invalidations;
  InvalidationPlan pending_rebuild;
  Version expected_version = 0;
};

inline InvalidationPlan AllPartitionsPlan() {
  InvalidationPlan plan;
  plan.scope = InvalidationScope::AllPartitions;
  return plan;
}

inline InvalidationPlan
PartitionSetPlanFromImages(const std::vector<StateImage> &images) {
  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  for (const auto &image : images) {
    plan.partitions.insert(
        static_cast<::accelerator_codegen::aux_state::PartitionKey>(
            image.partition));
  }
  return plan;
}

// RebuildPartition publishes replacement images for only the touched slots.
// Do not set pending_rebuild to AllPartitions — that would drop every other
// partition when the coordinator merges invalidation plans.
inline void FinishRebuildPartition(StagedMaintenanceOutcome &outcome) {
  outcome.action = MaintenanceAction::RebuildPartition;
  outcome.pending_rebuild = InvalidationPlan{};
  outcome.expected_version = 0;
  for (auto &image : outcome.updates) {
    image.version = 0;
  }
  outcome.invalidations = PartitionSetPlanFromImages(outcome.updates);
}

inline void FinishRebuildAll(StagedMaintenanceOutcome &outcome) {
  outcome.action = MaintenanceAction::RebuildAll;
  outcome.invalidations = AllPartitionsPlan();
  outcome.pending_rebuild = outcome.invalidations;
  outcome.updates.clear();
}

class AuxRowSink {
public:
  virtual ~AuxRowSink() = default;

  virtual void Begin(const OutputSchema &) = 0;
  virtual void WriteRow(const AuxRow &) = 0;
  virtual void End() = 0;
};

class StateBuilder {
public:
  virtual ~StateBuilder() = default;

  virtual void Write(StateImage image) = 0;
  virtual void Finish() = 0;
};

class AuxTxnBuffer {
public:
  virtual ~AuxTxnBuffer() = default;

  virtual bool HasPendingChanges() const = 0;
  virtual void Clear() = 0;
};

class AuxStateStore {
public:
  virtual ~AuxStateStore() = default;

  virtual StateSnapshot Read(const VisibilityToken &) const = 0;

  // Read only the requested published partitions. The default implementation
  // preserves compatibility for stores that have no efficient partition
  // selector; engine stores should override it to avoid serializing unrelated
  // images. This is a read filter, not an image cache.
  virtual StateSnapshot
  ReadPartitions(const VisibilityToken &visibility,
                 const std::vector<PartitionId> &partitions) const {
    auto snapshot = Read(visibility);
    snapshot.partitions.erase(
        std::remove_if(snapshot.partitions.begin(), snapshot.partitions.end(),
                       [&](const StateImage &image) {
                         return std::find(partitions.begin(), partitions.end(),
                                          image.partition) == partitions.end();
                       }),
        snapshot.partitions.end());
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }

  virtual ValidityResult CheckValidity(const QueryKey &,
                                       const VisibilityToken &) const = 0;

  // Invalidation is applied only by the coordinator's Commit path.
  // Implementations must not expose an invalidation as a mutation-visible side
  // effect of staging.
  virtual void Invalidate(const InvalidationPlan &) = 0;

  // The implementation must publish an already materialized image atomically
  // with respect to readers. A false result means the expected version lost a
  // race.
  virtual bool TryPublish(const StagedMaintenanceOutcome &,
                          Version expected_version) = 0;

  virtual void MarkTransactionDirty() {}
  virtual void ClearTransactionDirty() {}
};

class AuxEngineServices {
public:
  virtual ~AuxEngineServices() = default;

  virtual VisibilityToken CurrentVisibility() const = 0;
  virtual AuxStateStore &StateStore() = 0;

  virtual const maintenance::CostModelParams &MaintenanceCostModel() const;

  // Estimated number of auxiliary queries that will run before the next
  // write. Engine adapters may provide a live workload hint; the default is
  // retained for callers that do not yet expose workload telemetry.
  virtual double ExpectedQueriesBeforeNextWrite() const;

  // RebuildAll Commit path may call module Build() to materialize a fresh
  // image. RebuildPartition must be filled by StageMaintenance (touched
  // partitions only). Engines whose commit hook cannot safely run table scans
  // should override to false and invalidate only (next query rebuilds) when
  // updates are empty.
  virtual bool AllowsSynchronousRebuildOnCommit() const;

  virtual MaintenanceDecision ChooseMaintenance(
      const MaintenanceRequest &request) const;

  // Intra-query worker count for parallel aux rebuild/execute. Default is 1
  // (or ACCEL_AUX_THREADS when set). Engine adapters may map this to host
  // thread/worker settings.
  virtual std::size_t WorkerCount() const;
};

class AuxQueryModule {
public:
  virtual ~AuxQueryModule() = default;

  virtual AcceleratorId Id() const = 0;
  virtual std::vector<TableDependency> Dependencies() const = 0;

  virtual std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const = 0;
  virtual void Build(AuxEngineServices &, StateBuilder &) = 0;

  virtual InvalidationPlan InterpretMutation(const MutationBatch &) const = 0;
  virtual void AccumulateMutation(const MutationBatch &, AuxEngineServices &,
                                  AuxTxnBuffer &) = 0;
  virtual StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &,
                                                    const InvalidationPlan &,
                                                    AuxTxnBuffer &) = 0;

  virtual bool CheckValidity(AuxEngineServices &,
                             const QueryArguments &) const = 0;
  virtual void Execute(AuxEngineServices &, const QueryArguments &,
                       AuxRowSink &) = 0;
  virtual void ExecuteFallback(AuxEngineServices &, const QueryArguments &,
                               AuxRowSink &) = 0;
};

} // namespace accelerator_codegen::aux_state::core
