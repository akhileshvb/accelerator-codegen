// SCAFFOLDING
#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::core {

class AuxTransactionCoordinator {
public:
  void Register(std::shared_ptr<AuxQueryModule> module);
  void AttachServices(AuxEngineServices &services);

  void OnMutation(const MutationBatch &mutation);
  void BeginSubtransaction();
  void CommitSubtransaction();
  void AbortSubtransaction();
  void PreCommit();
  void Commit();
  void Abort();

  bool HasRegisteredModule(const AcceleratorId &id) const;
  std::size_t ModuleCount() const;

private:
  struct Entry {
    std::shared_ptr<AuxQueryModule> module;
    std::unique_ptr<AuxTxnBuffer> buffer;
    InvalidationPlan plan;
    StagedMaintenanceOutcome outcome;
    bool staged = false;
  };

  Entry &FindEntry(const AcceleratorId &id);
  const Entry &FindEntry(const AcceleratorId &id) const;
  void ClearTransactionState();
  void ApplyMutationLocked(const MutationBatch &mutation);

  AuxEngineServices *services = nullptr;
  std::vector<Entry> entries;
  std::unordered_map<AcceleratorId, std::size_t> entry_by_id;
  std::vector<std::vector<MutationBatch>> subtransaction_frames;
  mutable std::mutex mutex;
};

using AuxRegistry = AuxTransactionCoordinator;

} // namespace accelerator_codegen::aux_state::core
