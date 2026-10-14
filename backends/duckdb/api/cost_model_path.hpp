// GENERATED BACKEND
#pragma once

#include <string>

#include "catalog/query_catalog.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// The cost model the DuckDB runtime loads for `entry`, from the query's
// location (entry.cost_model): ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL when
// set; else the scale's or the default model, ACCEL_AUX_COST_MODEL_DIR (a subdirectory of the query's config dir) first;
// else the query's default path.
std::string SharedCostModelConfigPath(const SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::aux_state::engine::duckdb
