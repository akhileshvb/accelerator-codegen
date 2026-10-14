#pragma once

#include "sec_edgar/q6/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// Parallel variant. The serial module's Build() already delegates the heavy
// num x pre x sub aggregation to the engine through
// SecEdgarQ6InitialScan::ScanGroupAggregates, which DuckDB runs with all
// worker threads (PRAGMA threads / ACCEL_AUX_THREADS). Maintenance is the same
// incremental ApplyDelta path as the serial module: the state is engine- and
// thread-count independent, so the parallel store only differs by accelerator
// id.
class SecEdgarQ6ParallelModule : public SecEdgarQ6Module {
public:
  static constexpr const char *kId = core::SecEdgarQ6Layout::kParallelAccelerator;

  explicit SecEdgarQ6ParallelModule(SecEdgarQ6Config config,
                                    SecEdgarQ6InitialScan *initial_scan = nullptr)
      : SecEdgarQ6Module(config, initial_scan) {}

  core::AcceleratorId Id() const override { return kId; }
};

} // namespace accelerator_codegen::aux_state::query
