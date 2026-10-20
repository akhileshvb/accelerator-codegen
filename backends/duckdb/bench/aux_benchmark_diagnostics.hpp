#pragma once

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "substrate/aux/core/aux_store_diagnostics.hpp"

#include <iomanip>
#include <ostream>
#include <string>

namespace accelerator_codegen::benchmarks {

inline constexpr const char *kAuxStoreDiagnosticsCsvHeader =
    "aux_state_bytes,valid_partitions,total_slots,validity_slots_scanned_median,"
    "validity_scan_us_median,partitions_read_per_query_median,"
    "state_bytes_read_median,store_execute_calls";

inline void AppendStoreDiagnosticsCsv(
    std::ostream &out, uint64_t aux_state_bytes,
    const accelerator_codegen::aux_state::core::AuxStoreDiagnosticsSnapshot &diag) {
  out << aux_state_bytes << ',' << std::fixed << std::setprecision(2)
      << diag.valid_partitions << ',' << diag.total_slots << ','
      << diag.validity_slots_scanned_median << ','
      << diag.validity_scan_us_median << ','
      << diag.partitions_read_per_query_median << ','
      << diag.state_bytes_read_median << ',' << diag.execute_calls;
}

inline accelerator_codegen::aux_state::core::AuxStoreDiagnosticsSnapshot
CollectStoreDiagnostics(
    accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime &runtime) {
  return runtime.StoreDiagnosticsSnapshot();
}

inline uint64_t CollectAuxStateFootprintBytes(
    accelerator_codegen::aux_state::engine::duckdb::DuckdbRuntime &runtime) {
  return runtime.AuxStateFootprintBytes();
}

} // namespace accelerator_codegen::benchmarks
