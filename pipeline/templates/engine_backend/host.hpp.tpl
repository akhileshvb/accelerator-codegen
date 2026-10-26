// GENERATED BACKEND — the {{engine_name}} host: the runtime skeleton's EngineSlots for this engine.
// Spec: pipeline/prompts/shared/docs/engine_interfaces.md ("Backend contract"), layers.md.
//
// One host per database (per shard worker on a shipped placement). It owns the
// runtime::RuntimeHost and implements the slots S2-S6 + S8 below; S1 capture
// (engine/capture.*) and S7 entry points (api/) call into RuntimeHost /
// AcceleratorRuntime. No runtime flow lives here: commit ordering, gaps,
// catch-up, build scheduling, versions and thread pools are substrate.
#pragma once

#include <memory>

#include "substrate/aux/runtime/runtime_host.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

class {{EngineName}}Host final : public runtime::EngineSlots {
public:
  {{EngineName}}Host();
  ~{{EngineName}}Host() override;

  runtime::RuntimeHost &Runtime();

  // ---- S2 commit epoch ----
  runtime::Epoch PendingEpochLowerBound(uint64_t txn) const override;
  // ---- S4 version floor ----
  runtime::Epoch OldestReaderEpoch() const override;
  // ---- S5 build snapshot ----
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &tables,
                                     uint64_t timeout_us) override;
  std::unique_ptr<runtime::BuildSnapshot> OpenBuildSnapshot(runtime::AcceleratorRuntime &accelerator) override;
  // ---- S6 placement ----
  bool EngineWorkOnHostThread() const override;
  // ---- S8 engine scans ----
  void RunBuild(runtime::AcceleratorRuntime &accelerator, runtime::BuildSnapshot &snapshot,
                core::StateBuilder &builder) override;
  // ---- services for modules ----
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &accelerator) const override;

  // S3 reader epoch: the epoch the calling snapshot reads at (passed as
  // runtime::ReadOptions::epoch by the S7 entry point).
  runtime::Epoch ReaderEpoch() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
