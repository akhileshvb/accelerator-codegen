#pragma once

#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include <functional>
#include <string>

#include "accelerator_codegen/duckdb_scan_adapter.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Streams rows from a PostgreSQL table scan into DuckDB DataChunks using SPI.
// Filters from the NativeScanSpec are pushed into the generated SQL WHERE clause.
void ScanNativeSpec(const NativeScanSpec &scan_spec,
                    const std::function<void(::duckdb::DataChunk &)> &consumer);

} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
