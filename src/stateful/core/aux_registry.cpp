// SCAFFOLDING
#include "accelerator_codegen/stateful/core/aux_registry.hpp"

#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::core {
namespace {

struct CollectingStateBuilder final : StateBuilder {
  explicit CollectingStateBuilder(StagedMaintenanceOutcome &outcome_p)
      : outcome(outcome_p) {}

  void Write(StateImage image) override {
    outcome.updates.push_back(std::move(image));
  }

  void Finish() override { finished = true; }

  StagedMaintenanceOutcome &outcome;
  bool finished = false;
};

bool IsSyncRebuildAction(MaintenanceAction action) {
  return action == MaintenanceAction::RebuildAll ||
         action == MaintenanceAction::RebuildPartition;
}

void MaterializeSyncRebuild(AuxEngineServices &services, AuxQueryModule &module,
                            StagedMaintenanceOutcome &outcome) {
  CollectingStateBuilder builder(outcome);
  module.Build(services, builder);
  if (!builder.finished) {
    throw std::logic_error(
        "auxiliary rebuild Build() did not call Finish(): " + module.Id());
  }
  outcome.expected_version = 0;
  for (auto &image : outcome.updates) {
    image.version = 0;
  }
}

} // namespace

void AuxTransactionCoordinator::Register(
    std::shared_ptr<AuxQueryModule> module) {
  if (!module) {
    throw std::invalid_argument(
        "cannot register a null auxiliary query module");
  }
  std::lock_guard<std::mutex> guard(mutex);
  const auto id = module->Id();
  if (entry_by_id.find(id) != entry_by_id.end()) {
    throw std::invalid_argument("duplicate auxiliary query module: " + id);
  }
  Entry entry;
  entry.module = std::move(module);
  entry.buffer = entry.module->CreateTxnBuffer();
  if (!entry.buffer) {
    throw std::invalid_argument(
        "auxiliary query module returned a null transaction buffer: " + id);
  }
  entry_by_id.emplace(id, entries.size());
  entries.emplace_back(std::move(entry));
}

void AuxTransactionCoordinator::AttachServices(AuxEngineServices &services_p) {
  std::lock_guard<std::mutex> guard(mutex);
  services = &services_p;
}

void AuxTransactionCoordinator::OnMutation(const MutationBatch &mutation) {
  std::lock_guard<std::mutex> guard(mutex);
  if (!services) {
    throw std::logic_error(
        "auxiliary transaction coordinator has no engine services");
  }
  for (const auto &entry : entries) {
    for (const auto &dependency : entry.module->Dependencies()) {
      if (dependency.table == mutation.table) {
        services->StateStore().MarkTransactionDirty();
        goto marked_dirty;
      }
    }
  }
marked_dirty:
  if (!subtransaction_frames.empty()) {
    subtransaction_frames.back().push_back(mutation);
    return;
  }
  ApplyMutationLocked(mutation);
}

void AuxTransactionCoordinator::BeginSubtransaction() {
  std::lock_guard<std::mutex> guard(mutex);
  subtransaction_frames.emplace_back();
}

void AuxTransactionCoordinator::CommitSubtransaction() {
  std::lock_guard<std::mutex> guard(mutex);
  if (subtransaction_frames.empty()) {
    throw std::logic_error("no auxiliary subtransaction to commit");
  }
  auto frame = std::move(subtransaction_frames.back());
  subtransaction_frames.pop_back();
  if (!subtransaction_frames.empty()) {
    auto &parent = subtransaction_frames.back();
    parent.insert(parent.end(), std::make_move_iterator(frame.begin()),
                  std::make_move_iterator(frame.end()));
    return;
  }
  for (const auto &mutation : frame) {
    ApplyMutationLocked(mutation);
  }
}

void AuxTransactionCoordinator::AbortSubtransaction() {
  std::lock_guard<std::mutex> guard(mutex);
  if (subtransaction_frames.empty()) {
    throw std::logic_error("no auxiliary subtransaction to abort");
  }
  subtransaction_frames.pop_back();
}

void AuxTransactionCoordinator::ApplyMutationLocked(
    const MutationBatch &mutation) {
  for (auto &entry : entries) {
    bool dependent = false;
    for (const auto &dependency : entry.module->Dependencies()) {
      if (dependency.table == mutation.table) {
        dependent = true;
        break;
      }
    }
    if (!dependent) {
      continue;
    }
    MergeInvalidationPlan(entry.plan,
                          entry.module->InterpretMutation(mutation));
    entry.module->AccumulateMutation(mutation, *services, *entry.buffer);
    // Untyped CSV capture often fails module parsers, which then neither buffer
    // deltas nor mark rebuild. Force an invalidation plan so PreCommit cannot
    // no-op and leave a Valid-but-stale store after Commit.
    if (entry.plan.IsEmpty() && !entry.buffer->HasPendingChanges()) {
      MergeInvalidationPlan(entry.plan, AllPartitionsPlan());
    }
  }
}

void AuxTransactionCoordinator::PreCommit() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!services) {
    throw std::logic_error(
        "auxiliary transaction coordinator has no engine services");
  }
  if (!subtransaction_frames.empty()) {
    throw std::logic_error(
        "cannot precommit with an open auxiliary subtransaction");
  }
  for (auto &entry : entries) {
    if (!entry.buffer->HasPendingChanges() && entry.plan.IsEmpty()) {
      continue;
    }
    entry.outcome =
        entry.module->StageMaintenance(*services, entry.plan, *entry.buffer);
    entry.staged = true;
    if (IsSyncRebuildAction(entry.outcome.action)) {
      if (!services->AllowsSynchronousRebuildOnCommit()) {
        // Postgres: discard any PRE_COMMIT SPI rebuild images (they can miss
        // this xact's writes). Invalidate and let the next Execute EnsureBuilt
        // rescan after commit. ApplyDelta updates are not sync-rebuild.
        if (entry.outcome.action == MaintenanceAction::RebuildPartition) {
          if (entry.outcome.invalidations.IsEmpty() &&
              !entry.outcome.updates.empty()) {
            entry.outcome.invalidations =
                PartitionSetPlanFromImages(entry.outcome.updates);
          }
          if (entry.outcome.invalidations.IsEmpty()) {
            entry.outcome.action = MaintenanceAction::RebuildAll;
            entry.outcome.invalidations = AllPartitionsPlan();
          }
        } else if (entry.outcome.invalidations.IsEmpty()) {
          entry.outcome.invalidations = AllPartitionsPlan();
        }
        entry.outcome.updates.clear();
        entry.outcome.pending_rebuild = entry.outcome.invalidations;
      } else if (entry.outcome.updates.empty()) {
        // DuckDB (and similar): materialize sync rebuild when module left
        // updates empty.
        if (entry.outcome.action == MaintenanceAction::RebuildPartition) {
          entry.outcome.action = MaintenanceAction::RebuildAll;
        }
        MaterializeSyncRebuild(*services, *entry.module, entry.outcome);
      }
    }
    RecordStagedMaintenanceAction(entry.outcome.action);
  }
}

void AuxTransactionCoordinator::Commit() {
  std::lock_guard<std::mutex> guard(mutex);
  if (!services) {
    throw std::logic_error(
        "auxiliary transaction coordinator has no engine services");
  }
  for (auto &entry : entries) {
    if (!entry.staged) {
      continue;
    }

    auto action = entry.outcome.action;
    // RebuildAll used to invalidate only and rely on ExecuteFallback, which
    // rebuilds per query without publishing. Materialize a full Build into
    // updates when the module did not already supply them (e.g. SecEdgar Q2),
    // then invalidate once and publish so later queries hit.
    // RebuildPartition must not take this path: a full Build would rescan
    // every partition and Invalidate(All) would drop the untouched slots.
    // Modules that cannot rebuild locally leave updates empty; treat that as
    // RebuildAll so correctness is preserved.
    // InvalidateAndFallback stays lazy on purpose.
    // PostgreSQL (AllowsSynchronousRebuildOnCommit==false) stages Rebuild* in
    // PreCommit without SPI Build; empty updates → Invalidate here, then the
    // next Execute CheckValidity→EnsureBuilt. DuckDB may Materialize above.
    if (action == MaintenanceAction::RebuildPartition &&
        entry.outcome.updates.empty()) {
      action = MaintenanceAction::RebuildAll;
      entry.outcome.action = action;
    }
    if (action == MaintenanceAction::RebuildAll &&
        entry.outcome.updates.empty()) {
      if (services->AllowsSynchronousRebuildOnCommit()) {
        MaterializeSyncRebuild(*services, *entry.module, entry.outcome);
      } else if (entry.outcome.invalidations.IsEmpty()) {
        entry.outcome.invalidations = AllPartitionsPlan();
      }
    }

    if (IsSyncRebuildAction(action)) {
      InvalidationPlan plan = entry.outcome.invalidations;
      MergeInvalidationPlan(plan, entry.outcome.pending_rebuild);
      if (action == MaintenanceAction::RebuildAll) {
        // Dropping stale partitions is required even when a module already
        // filled updates without an invalidation plan (sets can shrink).
        if (plan.IsEmpty() && !entry.outcome.updates.empty()) {
          plan = AllPartitionsPlan();
        }
      } else if (action == MaintenanceAction::RebuildPartition) {
        // Never escalate to AllPartitions: other slots must stay Valid.
        if (plan.IsEmpty() && !entry.outcome.updates.empty()) {
          plan = PartitionSetPlanFromImages(entry.outcome.updates);
        }
        if (plan.scope ==
            ::accelerator_codegen::aux_state::InvalidationScope::AllPartitions) {
          plan = PartitionSetPlanFromImages(entry.outcome.updates);
        }
      }
      if (!plan.IsEmpty()) {
        services->StateStore().Invalidate(plan);
      }
      if (!entry.outcome.updates.empty()) {
        services->StateStore().TryPublish(entry.outcome, /*expected_version=*/0);
      }
      continue;
    }

    if (!entry.outcome.invalidations.IsEmpty()) {
      services->StateStore().Invalidate(entry.outcome.invalidations);
    }
    if (!entry.outcome.pending_rebuild.IsEmpty()) {
      services->StateStore().Invalidate(entry.outcome.pending_rebuild);
    }
    if (!entry.outcome.updates.empty()) {
      services->StateStore().TryPublish(entry.outcome,
                                        entry.outcome.expected_version);
    }
  }
  services->StateStore().ClearTransactionDirty();
  ClearTransactionState();
}

void AuxTransactionCoordinator::Abort() {
  std::lock_guard<std::mutex> guard(mutex);
  if (services) {
    services->StateStore().ClearTransactionDirty();
  }
  ClearTransactionState();
}

bool AuxTransactionCoordinator::HasRegisteredModule(
    const AcceleratorId &id) const {
  std::lock_guard<std::mutex> guard(mutex);
  return entry_by_id.find(id) != entry_by_id.end();
}

std::size_t AuxTransactionCoordinator::ModuleCount() const {
  std::lock_guard<std::mutex> guard(mutex);
  return entries.size();
}

std::optional<std::vector<int32_t>>
AuxTransactionCoordinator::MutationCaptureAttnums(TableId table) const {
  // Return the first registered module's projection for `table`. Postgres
  // capture fans out per Runtime backend, so each coordinator typically owns
  // one module — preserve that module's attnum order for dense Read*.
  std::lock_guard<std::mutex> guard(mutex);
  for (const auto &entry : entries) {
    if (!entry.module) {
      continue;
    }
    for (const auto &dep : entry.module->Dependencies()) {
      if (dep.table != table) {
        continue;
      }
      return dep.mutation_attnums;
    }
  }
  return std::nullopt;
}

bool AuxTransactionCoordinator::HasPendingWork() const {
  std::lock_guard<std::mutex> guard(mutex);
  if (!subtransaction_frames.empty()) {
    return true;
  }
  for (const auto &entry : entries) {
    if (entry.buffer && entry.buffer->HasPendingChanges()) {
      return true;
    }
    if (!entry.plan.IsEmpty()) {
      return true;
    }
    if (entry.staged) {
      return true;
    }
  }
  return false;
}

AuxTransactionCoordinator::Entry &
AuxTransactionCoordinator::FindEntry(const AcceleratorId &id) {
  return entries.at(entry_by_id.at(id));
}

const AuxTransactionCoordinator::Entry &
AuxTransactionCoordinator::FindEntry(const AcceleratorId &id) const {
  return entries.at(entry_by_id.at(id));
}

void AuxTransactionCoordinator::ClearTransactionState() {
  subtransaction_frames.clear();
  for (auto &entry : entries) {
    entry.buffer->Clear();
    entry.plan = InvalidationPlan();
    entry.outcome = StagedMaintenanceOutcome();
    entry.staged = false;
  }
}

} // namespace accelerator_codegen::aux_state::core
