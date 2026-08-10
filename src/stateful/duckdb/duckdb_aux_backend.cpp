#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

DuckDBAuxBackend::DuckDBAuxBackend(
    core::AuxTransactionCoordinator &coordinator_p,
    core::AuxStateStore &state_store_p,
    MutationTranslator mutation_translator_p, VisibilityCallback visibility_p,
    CostModelCallback cost_model_p,
    WorkloadCallback workload_p)
    : coordinator(coordinator_p), state_store(state_store_p),
      mutation_translator(std::move(mutation_translator_p)),
      visibility(std::move(visibility_p)),
      cost_model(cost_model_p
                     ? cost_model_p()
                     : core::LoadAuxCostModelJson(
                           std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL")
                               ? std::getenv("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL")
                               : "")),
      workload(std::move(workload_p)) {
  if (!mutation_translator || !visibility) {
    throw std::invalid_argument(
        "DuckDBAuxBackend requires mutation and visibility callbacks");
  }
  coordinator.AttachServices(*this);
}

void DuckDBAuxBackend::OnTableMutation(
    ::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) {
  coordinator.OnMutation(mutation_translator(event));
}

void DuckDBAuxBackend::CaptureMutation(const core::MutationBatch &mutation) {
  coordinator.OnMutation(mutation);
}

void DuckDBAuxBackend::TransactionPreCommit(::duckdb::MetaTransaction &,
                                            ::duckdb::ClientContext &) {
  coordinator.PreCommit();
}

void DuckDBAuxBackend::TransactionCommit(::duckdb::MetaTransaction &,
                                         ::duckdb::ClientContext &) {
  coordinator.Commit();
}

void DuckDBAuxBackend::TransactionRollback(::duckdb::MetaTransaction &,
                                           ::duckdb::ClientContext &) {
  coordinator.Abort();
}

core::VisibilityToken DuckDBAuxBackend::CurrentVisibility() const {
  return visibility();
}

core::AuxStateStore &DuckDBAuxBackend::StateStore() { return state_store; }

const maintenance::CostModelParams &
DuckDBAuxBackend::MaintenanceCostModel() const {
  return cost_model;
}

double DuckDBAuxBackend::ExpectedQueriesBeforeNextWrite() const {
  if (workload) {
    return std::max(0.0, workload());
  }
  const char *value =
      std::getenv("ACCELERATOR_CODEGEN_DUCKDB_EXPECTED_QUERIES_BEFORE_WRITE");
  return value && *value ? std::max(0.0, std::stod(value)) : 10.0;
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
