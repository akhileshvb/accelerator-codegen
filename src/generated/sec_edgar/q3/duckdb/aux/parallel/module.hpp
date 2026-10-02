#pragma once

#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "sec_edgar/q3/layout.hpp"

namespace accelerator_codegen::aux_state::query {

class SecEdgarQ3ParallelModule final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ3Layout::kParallelAccelerator;

  explicit SecEdgarQ3ParallelModule(SecEdgarQ3Config config,
                                    SecEdgarQ3InitialScan *initial_scan = nullptr)
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
  InterpretMutation(const core::MutationBatch &m) const override {
    return inner_.InterpretMutation(m);
  }
  void AccumulateMutation(const core::MutationBatch &m, core::AuxEngineServices &s,
                          core::AuxTxnBuffer &b) override {
    inner_.AccumulateMutation(m, s, b);
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
  SecEdgarQ3State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ3State &, core::AuxRowSink &) const;
  void EmitState(const SecEdgarQ3State &, core::StateBuilder &) const;

  SecEdgarQ3Module inner_;
  SecEdgarQ3InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
