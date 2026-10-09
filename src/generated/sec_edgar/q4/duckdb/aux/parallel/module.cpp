#include "sec_edgar/q4/duckdb/aux/parallel/module.hpp"

namespace accelerator_codegen::aux_state::query {

// The state now keeps every (stmt, uom) group, so the full build is one
// DuckDB-parallel SQL join + aggregation (see duckdb_scan.cpp): the earlier
// per-entry parallel num scan (sized to the ~1M EQ/USD keys) no longer applies.
SecEdgarQ4State
SecEdgarQ4ParallelModule::BuildState(core::AuxEngineServices &services) const {
  return SecEdgarQ4Module::BuildState(services);
}

} // namespace accelerator_codegen::aux_state::query
