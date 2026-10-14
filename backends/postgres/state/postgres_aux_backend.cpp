// GENERATED BACKEND
#include "backends/postgres/state/postgres_aux_backend.hpp"

#include "backends/postgres/state/aux_mutation_epoch.hpp"
#include "backends/postgres/engine/snapshot_token.hpp"

#include <algorithm>
#include <utility>

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresAuxBackend::PostgresAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p,
    CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p() : maintenance::CostModelParams{}),
      workload(std::move(workload_p)) {
  coordinator.AttachServices(*this);
}

void PostgresAuxBackend::CaptureMutation(
    const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void PostgresAuxBackend::CaptureMutationAtDepth(
    const core::MutationBatch &mutation, std::size_t depth) {
  coordinator.OnMutationAtDepth(mutation, depth);
}

void PostgresAuxBackend::OnSubCommit(std::size_t depth) {
  coordinator.CommitSubtransactionAtDepth(depth);
}

void PostgresAuxBackend::OnSubAbort(std::size_t depth) {
  coordinator.AbortSubtransactionAtDepth(depth);
}

void PostgresAuxBackend::OnPreCommit() {
  // Bump before Invalidate so mid-PRE_COMMIT readers pinned to the old epoch
  // see NotVisible/Invalid rather than a still-Valid stale image, and so any
  // reader that has not pinned yet and pins after this bump takes the new
  // epoch (cannot Hit Aux that will publish at a still-newer Commit epoch).
  BumpAuxMutationEpoch();
  coordinator.PreCommit();
}

void PostgresAuxBackend::OnCommit() {
  // TokenForSnapshot: bump epoch before TryPublish so readers pinned to an
  // older epoch cannot observe Aux published in this commit under old S.
  BumpAuxMutationEpoch();
  coordinator.Commit();
}

void PostgresAuxBackend::OnAbort() { coordinator.Abort(); }

core::VisibilityToken PostgresAuxBackend::CurrentVisibility() const {
  // Also pin here: any observation of active visibility freezes TokenForSnapshot
  // (covers paths that call CurrentVisibility without going through Runtime::Execute).
  if (visibility) {
    return visibility();
  }
  return PinTokenForActiveSnapshot();
}

core::AuxStateStore &PostgresAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
PostgresAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double PostgresAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  return workload ? std::max(0.0, workload()) : 10.0;
}

bool PostgresAuxBackend::AllowsSynchronousRebuildOnCommit() const {
  return false;
}

std::optional<std::vector<int32_t>>
PostgresAuxBackend::MutationCaptureAttnums(core::TableId table) const {
  return coordinator.MutationCaptureAttnums(table);
}

} // namespace accelerator_codegen::aux_state::engine::postgres
