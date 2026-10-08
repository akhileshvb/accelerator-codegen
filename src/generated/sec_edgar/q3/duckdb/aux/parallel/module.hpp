#pragma once

#include "sec_edgar/q3/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// The "parallel" Q3 accelerator shares the single-threaded module's state and
// maintenance (including ApplyDelta); it only publishes under its own
// accelerator id. Its (re)build is the same one-pass GROUP BY adsh aggregation,
// which DuckDB itself runs with PRAGMA threads workers.
class SecEdgarQ3ParallelModule final : public SecEdgarQ3Module {
public:
  static constexpr const char *kId = core::SecEdgarQ3Layout::kParallelAccelerator;

  explicit SecEdgarQ3ParallelModule(SecEdgarQ3Config config,
                                    SecEdgarQ3InitialScan *initial_scan = nullptr)
      : SecEdgarQ3Module(config, initial_scan, kId) {}
};

} // namespace accelerator_codegen::aux_state::query
