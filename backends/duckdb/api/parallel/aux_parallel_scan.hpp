#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>

#include "backends/duckdb/api/native_scan_compat.hpp"
#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"
#include "substrate/aux/state/invalidation_plan.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {
class ClientContext;
}

namespace accelerator_codegen {

inline std::string ChunkString(const duckdb::DataChunk &chunk, duckdb::idx_t column,
                               duckdb::idx_t row) {
	const auto value = chunk.GetValue(column, row);
	return value.IsNull() ? std::string() : value.ToString();
}

inline bool ChunkNull(const duckdb::DataChunk &chunk, duckdb::idx_t column,
                      duckdb::idx_t row) {
	return chunk.GetValue(column, row).IsNull();
}

inline int32_t ChunkInt32(const duckdb::DataChunk &chunk, duckdb::idx_t column,
                          duckdb::idx_t row) {
	const auto value = chunk.GetValue(column, row);
	if (value.IsNull()) {
		return 0;
	}
	try {
		return static_cast<int32_t>(value.GetValue<int64_t>());
	} catch (...) {
		return static_cast<int32_t>(std::stoll(value.ToString()));
	}
}

inline double ChunkDouble(const duckdb::DataChunk &chunk, duckdb::idx_t column,
                          duckdb::idx_t row, bool &is_null) {
	const auto value = chunk.GetValue(column, row);
	is_null = value.IsNull();
	if (is_null) {
		return 0.0;
	}
	if (value.type().id() == duckdb::LogicalTypeId::DOUBLE) {
		return value.GetValue<double>();
	}
	return std::stod(value.ToString());
}

::duckdb::ClientContext *DuckDBExecutionContext(aux_state::core::AuxEngineServices &services);

void RunParallelNativeScan(
    duckdb::ClientContext &context, const NativeScanSpec &spec,
    duckdb::idx_t worker_count,
    const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)>
        &consume);

bool ScanParallel(
    aux_state::core::AuxEngineServices &services, const NativeScanSpec &spec,
    const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)>
        &consume);

// ST modules publish images under sec_edgar_qN. Parallel kernels keep their
// own store and rebuild with the parallel Build() path. Do not publish ST
// ApplyDelta / serial rebuild payloads: ST incremental (especially Q2/Q4)
// does not reliably match DuckDB after writes when served from the parallel
// store.
inline aux_state::core::StagedMaintenanceOutcome AdoptParallelMaintenance(
    aux_state::core::StagedMaintenanceOutcome outcome,
    const char *accelerator_id,
    aux_state::core::AuxEngineServices &services) {
	using aux_state::core::MaintenanceAction;
	if (outcome.action == MaintenanceAction::ApplyDelta ||
	    outcome.action == MaintenanceAction::RebuildAll ||
	    outcome.action == MaintenanceAction::RebuildPartition) {
		outcome.action = MaintenanceAction::RebuildAll;
		outcome.updates.clear();
	}
	for (auto &image : outcome.updates) {
		image.accelerator = accelerator_id;
	}
	(void)services;
	return outcome;
}

} // namespace accelerator_codegen
