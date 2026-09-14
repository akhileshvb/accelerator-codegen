// GENERATED BACKEND — engine-local scan helper template (aux rebuild / contract)
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

// Engine-local base-table access for aux Build/rebuild and contract tests.
// NOT a portable DataChunk IR — rename / widen types per SDK hints + analysis.
struct {{EngineName}}ScanColumn {
  std::string name;
};

struct {{EngineName}}ScanSpec {
  std::string catalog_name;
  std::string schema_name;
  std::string table_name;
  std::vector<{{EngineName}}ScanColumn> projected_columns;
  // Optional residual filters — shape is engine-specific; keep simple for stubs.
};

// One projected row as opaque typed slots (analysis replaces with real types).
using {{EngineName}}ScanRowConsumer =
    std::function<void(const void *const *column_ptrs, std::size_t column_count)>;

// Stream projected base-table rows via native storage into typed callbacks.
// Forbidden: client-SQL round-trip on the hot path.
// Forbidden: converting into duckdb::DataChunk for "portable" kernels.
void ScanNativeBaseTableForRebuild(const {{EngineName}}ScanSpec &scan_spec,
                                   const {{EngineName}}ScanRowConsumer &consumer);

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
