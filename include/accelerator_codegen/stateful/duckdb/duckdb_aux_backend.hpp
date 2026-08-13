// GENERATED BACKEND
#pragma once

#include <functional>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "duckdb/accelerator/mutation_recorder.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

class DuckDBAuxBackend final : public ::duckdb::MutationConsumer,
                               public core::AuxEngineServices {
public:
  using MutationTranslator =
      std::function<core::MutationBatch(const ::duckdb::TableMutationEvent &)>;
  using VisibilityCallback = std::function<core::VisibilityToken()>;
  using CostModelCallback =
      std::function<maintenance::CostModelParams()>;
  using WorkloadCallback = std::function<double()>;

  DuckDBAuxBackend(core::AuxTransactionCoordinator &coordinator,
                   core::AuxStateStore &state_store,
                   MutationTranslator mutation_translator,
                   VisibilityCallback visibility,
                   CostModelCallback cost_model = {},
                   WorkloadCallback workload = {});

  void OnTableMutation(::duckdb::ClientContext &,
                       const ::duckdb::TableMutationEvent &) override;
  void CaptureMutation(const core::MutationBatch &mutation);
  void TransactionPreCommit(::duckdb::MetaTransaction &,
                            ::duckdb::ClientContext &) override;
  void TransactionCommit(::duckdb::MetaTransaction &,
                         ::duckdb::ClientContext &) override;
  void TransactionRollback(::duckdb::MetaTransaction &,
                           ::duckdb::ClientContext &) override;

  core::VisibilityToken CurrentVisibility() const override;
  core::AuxStateStore &StateStore() override;
  const maintenance::CostModelParams &MaintenanceCostModel() const override;
  double ExpectedQueriesBeforeNextWrite() const override;

private:
  core::AuxTransactionCoordinator &coordinator;
  core::AuxStateStore &state_store;
  MutationTranslator mutation_translator;
  VisibilityCallback visibility;
  maintenance::CostModelParams cost_model;
  WorkloadCallback workload;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
