#pragma once

#include <string>

#include "accelerator_codegen/aux_state/engine/shared/query_catalog.hpp"

namespace duckdb {
class Connection;
}

namespace accelerator_codegen::aux_state::engine {

// Shared-interface queries support two execution paths on every backend:
//   BaseKernel  - codegen fused-scan kernels reading native backend tables
//   AuxStateful - AuxQueryModule with incremental state
//
// DuckDB base kernels use NativeScanSpec + DuckDB storage scans.
// Postgres base kernels use NativeScanSpec + SPI scans (postgres_scan_adapter),
// then run the same KernelInstance implementations over DataChunks.
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

} // namespace accelerator_codegen::aux_state::engine
