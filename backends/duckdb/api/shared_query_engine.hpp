// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

#include "catalog/query_catalog.hpp"

namespace duckdb {
class Connection;
class Value;
}

namespace accelerator_codegen::aux_state::engine {

// Shared-interface queries support two execution paths on every backend:
//   BaseKernel  - per-engine codegen kernels reading that host's native tables
//   AuxStateful - AuxQueryModule with incremental state (shared MutationBatch /
//                 store contracts under stateful/core)
//
// There is no portable DataChunk IR. Base consume is engine-local:
//   DuckDB:  runtime/duckdb/native_scan.hpp (host-private DataChunk helpers)
//   Others:  runtime/{engine}/ helpers from engine_backend + accelerators
// Aux modules stay engine-agnostic; kernels under
// src/generated/{bench}/q{N}/{engine}/ do not.
class SharedQueryEngine {
public:
  static bool HasSharedQuery(SharedWorkloadKind workload, int query_id);
  static bool HasSharedQuery(const std::string &workload_name, int query_id);
};

void RegisterDuckdbAuxTableFunctions(::duckdb::Connection &connection);

// Infers and caches the accelerator table function's result schema (otherwise the
// first query binding it plans the base SQL, ~1-4 ms). For DuckdbRuntime::Build.
void PrimeAuxSchema(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry);

// An accelerator's arguments from DuckDB values, typed as the table function
// accel_<id>(...) types them (by the entry's argument types).
core::QueryArguments AuxArgumentsFromValues(const SharedQueryCatalogEntry &entry,
                                            const std::vector<::duckdb::Value> &values);

} // namespace accelerator_codegen::aux_state::engine
