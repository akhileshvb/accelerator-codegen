#pragma once

#include "sec_edgar/q2/duckdb/aux/module.hpp"

namespace accelerator_codegen::aux_state::query {

// Parallel variant: identical maintenance / read logic, published under the
// parallel accelerator id.  Build parallelism comes from the single columnar
// GROUP BY the scan source issues, which DuckDB executes with PRAGMA threads
// workers.
class SecEdgarQ2ParallelModule final : public SecEdgarQ2Module {
public:
  static constexpr const char *kId = core::SecEdgarQ2Layout::kParallelAccelerator;

  explicit SecEdgarQ2ParallelModule(SecEdgarQ2Config config,
                                    SecEdgarQ2InitialScan *initial_scan = nullptr)
      : SecEdgarQ2Module(config, initial_scan) {}

protected:
  const char *AcceleratorName() const override { return kId; }
};

} // namespace accelerator_codegen::aux_state::query
