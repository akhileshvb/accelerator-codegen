#include "accelerator_codegen/runtime/parallel/aux_parallel_scan.hpp"

#include <algorithm>

#include "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"
#include "accelerator_codegen/runtime/parallel/parallel_runtime.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "duckdb/main/client_context.hpp"

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)
#include "accelerator_codegen/runtime/postgres/scan_adapter.hpp"
#endif

namespace accelerator_codegen {

::duckdb::ClientContext *
DuckDBExecutionContext(aux_state::core::AuxEngineServices &services) {
	auto *backend = dynamic_cast<aux_state::engine::duckdb::DuckDBAuxBackend *>(&services);
	return backend ? backend->ExecutionContext() : nullptr;
}

void RunParallelNativeScan(
    duckdb::ClientContext &context, const NativeScanSpec &spec,
    duckdb::idx_t worker_count,
    const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)>
        &consume) {
	const bool started = !context.transaction.HasActiveTransaction();
	if (started) {
		context.transaction.BeginTransaction();
	}
	try {
		auto bind_data = BindNativeScan(context, spec);
		RunParallelChunkScan(context, *bind_data, worker_count, consume);
		if (started) {
			context.transaction.Commit();
		}
	} catch (...) {
		if (started && context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(nullptr);
		}
		throw;
	}
}

bool ScanSecEdgarParallel(
    aux_state::core::AuxEngineServices &services, const NativeScanSpec &spec,
    const std::function<void(duckdb::idx_t worker_id, duckdb::DataChunk &chunk)>
        &consume) {
	const auto worker_count =
	    static_cast<duckdb::idx_t>(std::max<std::size_t>(1, services.WorkerCount()));
	if (auto *context = DuckDBExecutionContext(services)) {
		RunParallelNativeScan(*context, spec, worker_count, consume);
		return true;
	}
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)
	aux_state::engine::postgres::RunParallelNativeScan(spec, worker_count, consume);
	return true;
#else
	(void)spec;
	(void)consume;
	(void)worker_count;
	return false;
#endif
}

} // namespace accelerator_codegen
