// SCAFFOLDING
// The engine-neutral vocabulary of auxiliary state: values, state images, the
// store and services interfaces, and the query-module interface.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"
#include "substrate/aux/state/invalidation_plan.hpp"

namespace accelerator_codegen::aux_state::core {

using AcceleratorId = std::string;
using TableId = uint64_t;
using ColumnId = uint32_t;
using PartitionId = uint64_t;
using Version = uint64_t;

// Reader wait for a pending commit: unbounded unless ACCEL_AUX_PENDING_WAIT_US bounds it.
constexpr uint64_t kWaitForPublish = ~uint64_t {0};
inline uint64_t PendingWaitMicrosFromEnv() {
  const char *value = std::getenv("ACCEL_AUX_PENDING_WAIT_US");
  return value && *value ? std::strtoull(value, nullptr, 10) : kWaitForPublish;
}

// Set when the process serving reads shuts down: a reader waiting for a pending commit stops waiting (the
// writer that would end it may be gone) and finds no state.
inline std::atomic<bool> g_pending_waits_cancelled{false};

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
  // Last: AuxType values and AuxPayload indices are part of the wire format.
  ScaledDecimal128,
};

struct ScaledDecimal {
  int64_t value = 0;
  int32_t scale = 0;

  bool operator==(const ScaledDecimal &other) const {
    return value == other.value && scale == other.scale;
  }
};

// DECIMAL(38, scale): the unscaled value is a signed 128-bit integer.
struct ScaledDecimal128 {
  __int128 value = 0;
  int32_t scale = 0;

  bool operator==(const ScaledDecimal128 &other) const {
    return value == other.value && scale == other.scale;
  }
};

// Exact decimal text, e.g. (-12345, scale 2) -> "-123.45"; a negative scale
// appends zeros.
inline std::string ScaledDecimal128ToString(__int128 value, int32_t scale) {
  const bool negative = value < 0;
  unsigned __int128 mag = negative ? (~static_cast<unsigned __int128>(value)) + 1
                                   : static_cast<unsigned __int128>(value);
  std::string digits;
  do {
    digits.push_back(static_cast<char>('0' + static_cast<int>(mag % 10)));
    mag /= 10;
  } while (mag != 0);
  if (scale < 0) {
    if (value != 0) {
      digits.insert(digits.begin(), static_cast<size_t>(-scale), '0');
    }
  } else if (scale > 0) {
    while (digits.size() <= static_cast<size_t>(scale)) {
      digits.push_back('0');
    }
    digits.insert(static_cast<size_t>(scale), ".");
  }
  if (negative) {
    digits.push_back('-');
  }
  return std::string(digits.rbegin(), digits.rend());
}

struct AuxDate {
  int32_t days_since_epoch = 0;

  bool operator==(const AuxDate &other) const {
    return days_since_epoch == other.days_since_epoch;
  }
};

using AuxPayload = std::variant<bool, int64_t, uint64_t, ScaledDecimal, double,
                                AuxDate, std::string, ScaledDecimal128>;

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

  // The module's immutable typed state, opaque to the substrate; an engine
  // that crosses a process boundary encodes it in its own adapter.
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

// A reader's epoch meaning "no snapshot: the newest version". Epoch 0 is a
// snapshot like any other (it sees no commit).
inline constexpr Version kNewestVersion = std::numeric_limits<Version>::max();

struct VisibilityToken {
  Version mutation_epoch = kNewestVersion;
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
  // Column ids in the *projected* mutation AuxRow (0..n-1 after capture
  // projection), used by InterpretMutation for watched-column filters.
  std::vector<ColumnId> watched_columns;
  // 0-based physical attribute numbers from the base table, in the order the
  // module's AccumulateMutation/Read* helpers expect. Empty means capture the
  // full row (all non-dropped attributes).
  std::vector<int32_t> mutation_attnums;
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

// RebuildPartition publishes replacement images for only the touched
// partitions, so pending_rebuild stays empty: AllPartitions would drop every
// other partition when the runtime merges invalidation plans.
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

// Rebuilds everything; a decision other than ApplyDelta/RebuildPartition (such as
// InvalidateAndRebuildOnRead) stays the outcome's action.
inline void FinishRebuildAll(StagedMaintenanceOutcome &outcome, const MaintenanceDecision &decision) {
  FinishRebuildAll(outcome);
  if (decision.action != MaintenanceAction::ApplyDelta &&
      decision.action != MaintenanceAction::RebuildPartition) {
    outcome.action = decision.action;
  }
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

  // Only the requested published partitions. The default filters Read();
  // stores with a partition index should override it.
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

  // Validity plus immutable handles for every required partition, in one
  // observation. Default: CheckValidity, then ReadPartitions, which must
  // return them all; stores should override it to make it atomic.
  virtual StateSnapshot
  ReadHitPartitions(const VisibilityToken &visibility,
                    const QueryKey &accel_key,
                    const std::vector<PartitionId> &partitions) const {
    StateSnapshot miss;
    miss.visibility = visibility;
    miss.available = false;
    if (!CheckValidity(accel_key, visibility).IsValid()) {
      return miss;
    }
    auto snapshot = ReadPartitions(visibility, partitions);
    if (snapshot.partitions.size() != partitions.size()) {
      miss.partitions.clear();
      return miss;
    }
    snapshot.available = true;
    return snapshot;
  }

  virtual ValidityResult CheckValidity(const QueryKey &,
                                       const VisibilityToken &) const = 0;

  // A reader that finds the state being rebuilt waits for it (up to the read
  // ceiling) instead of weighing the wait against its SQL.
  virtual bool WaitsForRebuild() const { return false; }

  // Invalid for every snapshot. (Commits go through BeginCommit /
  // FinishCommit below.)
  virtual void Invalidate(const InvalidationPlan &) = 0;

  // The implementation must publish an already materialized image atomically
  // with respect to readers. A false result means the expected version lost a
  // race.
  virtual bool TryPublish(const StagedMaintenanceOutcome &,
                          Version expected_version) = 0;

  // Commit protocol, driven by AcceleratorRuntime, per accelerator.
  // VersionedAuxStore implements versions; the defaults are for stores
  // without them, which keep their current state until FinishCommit (they
  // cannot hold a commit back from readers in between).
  //
  // BeginCommit: the accelerator's state is about to change (`plan`: the
  // partitions the commit touches). Readers whose snapshot may include the
  // commit wait for FinishCommit; older snapshots keep reading theirs.
  virtual void BeginCommit(const AcceleratorId &, const InvalidationPlan &) {}
  // FinishCommit: one new state, current from this commit on: `invalidate`
  // applied, then `updates` published, together. `expected_version` is the
  // module's (StagedMaintenanceOutcome::expected_version; 0: none) for stores
  // that check it on publish.
  virtual void FinishCommit(const AcceleratorId &,
                            const InvalidationPlan &invalidate,
                            const std::vector<StateImage> &updates,
                            Version expected_version = 0) {
    if (!invalidate.IsEmpty()) {
      Invalidate(invalidate);
    }
    if (!updates.empty()) {
      StagedMaintenanceOutcome outcome;
      outcome.updates = updates;
      outcome.expected_version = expected_version;
      TryPublish(outcome, expected_version);
    }
  }
  // After BeginCommit, instead of FinishCommit: nothing changes.
  virtual void AbortCommit(const AcceleratorId &) {}
  // Every accelerator of the engine commit at `epoch` has published (or not):
  // a store that assembles one version across accelerators installs it now.
  virtual void EndCommit(uint64_t /*epoch*/) {}
};

class AuxEngineServices {
public:
  virtual ~AuxEngineServices() = default;

  virtual VisibilityToken CurrentVisibility() const = 0;
  virtual AuxStateStore &StateStore() = 0;

  virtual const maintenance::CostModelParams &MaintenanceCostModel() const;

  // Estimated number of auxiliary queries that will run before the next
  // write: a workload hint (default 10).
  virtual double ExpectedQueriesBeforeNextWrite() const;

  // Rows in the dependency tables now; 0 if unknown. With the cost model's
  // calibrated_scale_rows it scales the ApplyDelta cap and the rebuild costs.
  virtual uint64_t DataScaleRows() const;

  virtual MaintenanceDecision ChooseMaintenance(
      const MaintenanceRequest &request) const;

  // Worker count for parallel build/execute. Default: ACCEL_AUX_THREADS, else 1.
  virtual std::size_t WorkerCount() const;

  // Runs fn(i) for i in [0, n) on the engine's thread pool (the calling thread
  // participates; total threads stay within the budget WorkerCount() reports)
  // and returns when all are done, rethrowing the first exception. Execute
  // paths that scan much state use it with one partial result per task,
  // merged in index order so results match a serial run. Default: serial.
  virtual void ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const;

  // Answers the query from the engine's SQL at the reader's snapshot, at the
  // cost of running that SQL. False: the engine has no SQL for this query.
  virtual bool AnswerFromSql(const QueryArguments &arguments, AuxRowSink &sink);
};

// One write shape for calibrating an accelerator's maintenance costs
// (AuxQueryModule::CalibrationWrites). ANSI SQL, run by each engine's
// calibration hook. `apply` changes rows on columns the accelerator depends
// on and `revert` restores them (same rows, same shape); `control` and
// `control_revert` write the same rows on a column it does not depend on, so
// their cost is what every action shares (the engine's own write).
struct CalibrationWrite {
  std::string name;
  std::string apply;
  std::string revert;
  std::string control;
  std::string control_revert;
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
  // The answer when the state cannot serve: the engine's SQL by default.
  virtual void ExecuteFallback(AuxEngineServices &, const QueryArguments &,
                               AuxRowSink &);

  // Writes of increasing size and spread over this accelerator's partitions,
  // for core::CalibrateFromWrites (empty: none; the accelerator keeps its
  // configured cost model).
  virtual std::vector<CalibrationWrite> CalibrationWrites() const { return {}; }
};

} // namespace accelerator_codegen::aux_state::core
