#pragma once

#include "sec_edgar/q1/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

class SecEdgarQ1ParallelModule final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ1Layout::kParallelAccelerator;

  explicit SecEdgarQ1ParallelModule(SecEdgarQ1Config config,
                                    SecEdgarQ1InitialScan *initial_scan = nullptr)
      : inner_(config, initial_scan), initial_scan(initial_scan) {}

  core::AcceleratorId Id() const override { return kId; }
  std::vector<core::TableDependency> Dependencies() const override {
    return inner_.Dependencies();
  }
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return inner_.CreateTxnBuffer();
  }

  void Build(core::AuxEngineServices &, core::StateBuilder &) override;
  core::InvalidationPlan
  InterpretMutation(const core::MutationBatch &mutation) const override {
    return inner_.InterpretMutation(mutation);
  }
  void AccumulateMutation(const core::MutationBatch &mutation,
                          core::AuxEngineServices &services,
                          core::AuxTxnBuffer &buffer) override {
    inner_.AccumulateMutation(mutation, services, buffer);
  }
  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override;

  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override;
  void Execute(core::AuxEngineServices &, const core::QueryArguments &,
               core::AuxRowSink &) override;
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override;

private:
  SecEdgarQ1State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ1State &, core::AuxRowSink &) const;
  void EmitState(const SecEdgarQ1State &, core::StateBuilder &) const;

  SecEdgarQ1Module inner_;
  SecEdgarQ1InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
