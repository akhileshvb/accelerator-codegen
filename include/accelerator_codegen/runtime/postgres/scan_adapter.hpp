// GENERATED BACKEND
#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <functional>

#include "accelerator_codegen/runtime/native_scan.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Postgres backend adapter for the shared NativeScanSpec contract.
// Streams base-table rows via the table AM (seqscan + residual filters) into
// DuckDB DataChunks — same IR as DuckDB's ScanNativeBaseTable.
void ScanNativeBaseTable(const NativeScanSpec &scan_spec,
                         const NativeScanChunkConsumer &consumer);

// Main-thread table AM scan with optional worker-side chunk processing.
// PostgreSQL backends are not MT-safe: the heap scan stays on the calling
// thread; workers only process copied DataChunks (no palloc/ereport).
void RunParallelNativeScan(
    const NativeScanSpec &scan_spec, ::duckdb::idx_t worker_count,
    const std::function<void(::duckdb::idx_t worker_id, ::duckdb::DataChunk &chunk)>
        &consume);

// PostgreSQL xact callbacks must not spawn std::thread (SIGSEGV on COMMIT).
void EnterPostgresXactCallback();
void LeavePostgresXactCallback();

// Back-compat alias used by generated kernel_runner.
inline void ScanNativeSpec(const NativeScanSpec &scan_spec,
                           const NativeScanChunkConsumer &consumer) {
	ScanNativeBaseTable(scan_spec, consumer);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
