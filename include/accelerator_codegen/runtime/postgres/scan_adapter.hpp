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

// Back-compat alias used by generated kernel_runner.
inline void ScanNativeSpec(const NativeScanSpec &scan_spec,
                           const NativeScanChunkConsumer &consumer) {
	ScanNativeBaseTable(scan_spec, consumer);
}

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
