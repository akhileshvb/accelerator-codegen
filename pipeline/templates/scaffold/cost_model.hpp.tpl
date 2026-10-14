// GENERATED QUERY: where {{benchmark_name}} Q{{query_number}} keeps its maintenance cost models (every engine).
// Pass {{cost_model_const}} to the engine's registration: the DuckDB catalog entry
// (SharedQueryCatalogEntry::cost_model in duckdb/aux/register.cpp) and the
// Postgres / MySQL Runtime constructor. The JSON files live in {{cost_model_dir}}/
// ({{cost_model_stem}}[_<engine>][_sf<N>].json; Verus variants under verus/).
#pragma once

#include "substrate/aux/core/aux_cost_model_path.hpp"

namespace accelerator_codegen::aux_state::core {

inline constexpr AuxCostModelLocation {{cost_model_const}}{"{{cost_model_dir}}", "{{cost_model_stem}}"};

} // namespace accelerator_codegen::aux_state::core
