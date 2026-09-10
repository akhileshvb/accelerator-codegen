// SCAFFOLDING
//
// Cross-engine scan IR is intentionally NOT defined here.
 // - Aux/txn/store contracts live under stateful/core (MutationBatch, etc.).
 // - Base-table streaming into kernels is per-engine:
 //     DuckDB:  runtime/duckdb/native_scan.hpp (DataChunk-shaped, host-private)
 //     Others:  runtime/{engine}/ helpers owned by engine_backend + accelerators
 //
 // Do not reintroduce a shared duckdb::DataChunk consumer as substrate API.
#pragma once

// Compatibility include for legacy duckdb host code that still pulls this path.
 // Prefer including runtime/duckdb/native_scan.hpp directly in new duckdb code.
#if defined(__has_include)
#if __has_include("accelerator_codegen/runtime/duckdb/native_scan.hpp")
#include "accelerator_codegen/runtime/duckdb/native_scan.hpp"
#endif
#endif
