#pragma once

#include "sec_edgar/q24/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

class SecEdgarQ24ParallelModule final : public core::AuxQueryModule {
public:
  static constexpr const char *kId = core::SecEdgarQ24Layout::kParallelAccelerator;

  explicit SecEdgarQ24ParallelModule(SecEdgarQ24Config config,
                                     SecEdgarQ24InitialScan *initial_scan = nullptr)
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
  SecEdgarQ24State BuildState(core::AuxEngineServices &) const;
  void EmitOutput(const SecEdgarQ24State &, core::AuxRowSink &) const;
  void EmitState(const SecEdgarQ24State &, core::StateBuilder &) const;

  SecEdgarQ24Module inner_;
  SecEdgarQ24InitialScan *initial_scan = nullptr;
};

} // namespace accelerator_codegen::aux_state::query
