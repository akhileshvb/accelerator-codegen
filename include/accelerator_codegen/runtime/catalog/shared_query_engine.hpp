// SCAFFOLDING
#pragma once

#include <string>

#include "catalog/query_catalog.hpp"

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state::engine {

// Shared-interface queries support two execution paths on every backend:
//   BaseKernel  - codegen fused-scan kernels reading native backend tables
//   AuxStateful - AuxQueryModule with incremental state
//
// Both engines implement the same NativeScanSpec contract (see
// runtime/native_scan.hpp): stream projected base-table rows as DataChunks.
//   DuckDB:   ScanNativeBaseTable via DataTable storage scans
//   Postgres: ScanNativeBaseTable via table AM seqscan + residual filters
// Kernels consume those DataChunks through KernelInstance.
enum class SharedQueryPath {
  BaseKernel,
  AuxStateful,
};

class SharedQueryEngine {
public:
  static const char *KernelSql(const SharedQueryCatalogEntry &entry,
                               SharedQueryPath path);

  static bool HasSharedQuery(SharedWorkloadKind workload, int query_id);
  static bool HasSharedQuery(const std::string &workload_name, int query_id);
};

void RegisterDuckdbAuxTableFunctions(::duckdb::Connection &connection);

#ifdef ACCEL_ENABLE_PARALLEL_AUX
void RegisterDuckdbAuxParallelTableFunctions(::duckdb::Connection &connection);
#endif

} // namespace accelerator_codegen::aux_state::engine
