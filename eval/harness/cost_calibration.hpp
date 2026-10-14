// SCAFFOLDING
#pragma once

#include <string>

#include "substrate/aux/core/aux_calibration.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

// The cost model the DuckDB runtime loads for `entry`, from the query's
// location (entry.cost_model): ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL when
// set; else the scale's or the default model, ACCEL_AUX_COST_MODEL_DIR (a subdirectory of the query's config dir) first;
// else the query's default path.
std::string SharedCostModelConfigPath(const aux_engine::SharedQueryCatalogEntry &entry);

core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const aux_engine::SharedQueryCatalogEntry &entry,
    const core::AuxCalibrationConfig &config = {});

void WriteSharedQueryCostModel(const std::string &path,
                               const core::AuxCalibrationResult &result,
                               const aux_engine::SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::aux_state::harness
