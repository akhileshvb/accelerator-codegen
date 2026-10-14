// SCAFFOLDING
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

class AuxTransactionCoordinator {
public:
  void Register(std::shared_ptr<AuxQueryModule> module);
  void AttachServices(AuxEngineServices &services);

  void OnMutation(const MutationBatch &mutation);
  void BeginSubtransaction();
  void CommitSubtransaction();
  void AbortSubtransaction();
  // Engine-driven subtransactions (PostgreSQL SAVEPOINT / exception blocks):
  // `depth` is the engine nesting depth (0 = top level). Frames are opened
  // lazily, so a coordinator first used inside a subtransaction stays aligned.
  void OnMutationAtDepth(const MutationBatch &mutation, std::size_t depth);
  void CommitSubtransactionAtDepth(std::size_t depth);
  void AbortSubtransactionAtDepth(std::size_t depth);
  void PreCommit();
  void Commit();
  void Abort();

  bool HasPendingWork() const;

  // Projection for mutation capture on `table`: nullopt if no module depends
  // on it; empty vector means full-row capture; otherwise physical attnums in
  // AuxRow order.
  std::optional<std::vector<int32_t>>
  MutationCaptureAttnums(TableId table) const;

private:
  struct Entry {
    std::shared_ptr<AuxQueryModule> module;
    std::unique_ptr<AuxTxnBuffer> buffer;
    InvalidationPlan plan;
    StagedMaintenanceOutcome outcome;
    bool staged = false;
    // BeginCommit was called at PRE_COMMIT (resolved at COMMIT / ABORT), for
    // these partitions.
    InvalidationPlan began_plan;
    bool began = false;
  };

  Entry &FindEntry(const AcceleratorId &id);
  const Entry &FindEntry(const AcceleratorId &id) const;
  void ClearTransactionState();
  void CommitLocked();
  void ApplyMutationLocked(const MutationBatch &mutation);
  void MarkDirtyIfDependentLocked(const MutationBatch &mutation);

  AuxEngineServices *services = nullptr;
  std::vector<Entry> entries;
  std::unordered_map<AcceleratorId, std::size_t> entry_by_id;
  std::vector<std::vector<MutationBatch>> subtransaction_frames;
  // Time spent maintaining state in this transaction's PreCommit + Commit
  // (recorded in the maintenance decision log at COMMIT).
  double maintenance_us = 0.0;
  mutable std::mutex mutex;
};

using AuxRegistry = AuxTransactionCoordinator;

} // namespace accelerator_codegen::aux_state::core
