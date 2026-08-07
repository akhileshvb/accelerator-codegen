#include "accelerator_codegen/aux_state/core/aux_registry.hpp"

#include <iterator>
#include <stdexcept>

namespace accelerator_codegen::aux_state::core {

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
