// SCAFFOLDING
#pragma once

#include <string>

#include "accelerator_codegen/aux_state/core/aux_calibration.hpp"
#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

std::string SharedCostModelConfigPath(aux_engine::SharedWorkloadKind workload, int query_id);

core::AuxCalibrationResult CalibrateSharedQueryCostModel(
    duckdb::Connection &connection, const aux_engine::SharedQueryCatalogEntry &entry,
    const core::AuxCalibrationConfig &config = {});

void WriteSharedQueryCostModel(const std::string &path,
                               const core::AuxCalibrationResult &result,
                               const aux_engine::SharedQueryCatalogEntry &entry);

} // namespace accelerator_codegen::aux_state::harness
