// GENERATED BACKEND — engine-local scan helper (Postgres native table-AM)
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::runtime::postgres {

struct PostgresScanSpec {
  std::string table_name;
  std::vector<std::string> projected_columns;
  std::string filter_column;
  std::string filter_op;   // "eq"
  std::string filter_value;
};

using PostgresScanRowConsumer = std::function<void(
    const std::vector<const char *> &column_names,
    const std::vector<const char *> &column_values,
    int column_count)>;

// Scan a base table using native Postgres table-AM (table_openrv,
// table_beginscan, table_scan_getnextslot). No client-SQL round-trip.
// No portable chunk IR.
void ScanNativeTable(const PostgresScanSpec &spec,
                     const PostgresScanRowConsumer &consumer);

} // namespace accelerator_codegen::aux_state::runtime::postgres
