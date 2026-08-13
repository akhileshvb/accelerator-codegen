// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const aux_engine::SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated);

void RequireSharedMatchesBase(duckdb::Connection &connection,
                              aux_engine::duckdb::DuckdbRuntime &runtime,
                              const aux_engine::SharedQueryCatalogEntry &entry);

void RunDifferentialCheck(const std::string &db_path,
                          const aux_engine::SharedQueryCatalogEntry &entry);

void RunIncrementalDifferentialCheck(const std::string &db_path,
                                     const aux_engine::SharedQueryCatalogEntry &entry);

// Apply alternating mini-chunk RF2/RF1 via DuckdbRuntime::ApplyRfChunk, then
// require Execute matches DuckDB base_sql after each chunk. db_path must be a
// writable work copy (mutations are permanent).
void RunRfChunkDifferentialCheck(const std::string &db_path,
                                 const aux_engine::SharedQueryCatalogEntry &entry,
                                 int num_chunks = 4, int order_count = 1);

} // namespace accelerator_codegen::aux_state::harness
