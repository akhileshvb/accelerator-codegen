#include "sec_edgar/q2/duckdb/aux/parallel/module.hpp"

// SecEdgarQ2ParallelModule is header-only (see module.hpp); this translation
// unit exists so the parallel aux source glob keeps one object per query.
namespace accelerator_codegen::aux_state::query {
static_assert(sizeof(SecEdgarQ2ParallelModule) > 0, "parallel module is complete");
} // namespace accelerator_codegen::aux_state::query
