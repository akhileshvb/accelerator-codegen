// SCAFFOLDING
#pragma once

#include <string>

#include "backends/duckdb/api/cost_model_path.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

// The cost model the DuckDB runtime loads for an entry: the backend's lookup
// policy (backends/duckdb/api/cost_model_path.hpp).
using aux_engine::duckdb::SharedCostModelConfigPath;

// Calibrated on the query bound to `arguments`.
core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const aux_engine::SharedQueryCatalogEntry &entry,
    const core::QueryArguments &arguments, const core::AuxCalibrationConfig &config = {});

void WriteSharedQueryCostModel(const std::string &path,
                               const core::AuxCalibrationResult &result,
                               const aux_engine::SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::aux_state::harness
