// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION) && \
    defined(ACCEL_ENABLE_PARALLEL_AUX)
extern int accel_aux_worker_count;
extern "C" int max_parallel_workers_per_gather;
#endif

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresAuxBackend::PostgresAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    VisibilityCallback visibility_p, CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p ? cost_model_p()
                              : maintenance::CostModelParams{}),
      workload(std::move(workload_p)) {
  if (!visibility) {
    throw std::invalid_argument(
        "PostgresAuxBackend requires a visibility callback");
  }
  coordinator.AttachServices(*this);
}

void PostgresAuxBackend::CaptureStatement(const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void PostgresAuxBackend::OnSubtransactionStart() {
  coordinator.BeginSubtransaction();
}

void PostgresAuxBackend::OnSubtransactionCommit() {
  coordinator.CommitSubtransaction();
}

void PostgresAuxBackend::OnSubtransactionAbort() {
  coordinator.AbortSubtransaction();
}

void PostgresAuxBackend::OnPreCommit() {
  coordinator.PreCommit();
}

void PostgresAuxBackend::OnCommit() {
  coordinator.Commit();
}

void PostgresAuxBackend::OnAbort() { coordinator.Abort(); }

core::VisibilityToken PostgresAuxBackend::CurrentVisibility() const {
  return visibility();
}

core::AuxStateStore &PostgresAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
PostgresAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double PostgresAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  if (workload) {
    return std::max(0.0, workload());
  }
  const char *value = std::getenv(
      "ACCELERATOR_CODEGEN_POSTGRES_EXPECTED_QUERIES_BEFORE_WRITE");
  return value && *value ? std::max(0.0, std::stod(value)) : 10.0;
}

bool PostgresAuxBackend::AllowsSynchronousRebuildOnCommit() const {
  // SPI/table scans need an ActiveSnapshot; Commit callbacks do not have one.
  return false;
}

std::size_t PostgresAuxBackend::WorkerCount() const {
  const char *value = std::getenv("ACCEL_AUX_THREADS");
  if (value && *value) {
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed > 0) {
      return static_cast<std::size_t>(parsed);
    }
  }
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION) && \
    defined(ACCEL_ENABLE_PARALLEL_AUX)
  if (::accel_aux_worker_count > 0) {
    return static_cast<std::size_t>(::accel_aux_worker_count);
  }
  if (::max_parallel_workers_per_gather > 0) {
    return static_cast<std::size_t>(::max_parallel_workers_per_gather);
  }
#endif
  return core::AuxEngineServices::WorkerCount();
}

} // namespace accelerator_codegen::aux_state::engine::postgres
