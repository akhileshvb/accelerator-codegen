#pragma once

#include "sec_edgar/q4/common/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// Same state, maintenance and read path as SecEdgarQ4Module; only the full
// (re)build differs: the num aggregation runs as a parallel native scan.
class SecEdgarQ4ParallelModule : public SecEdgarQ4Module {
public:
  static constexpr const char *kId = core::SecEdgarQ4Layout::kParallelAccelerator;

  explicit SecEdgarQ4ParallelModule(SecEdgarQ4Config config,
                                    SecEdgarQ4InitialScan *initial_scan = nullptr)
      : SecEdgarQ4Module(config, initial_scan) {}

  core::AcceleratorId Id() const override { return kId; }

protected:
  SecEdgarQ4State BuildState(core::AuxEngineServices &) const override;
};

} // namespace accelerator_codegen::aux_state::query
