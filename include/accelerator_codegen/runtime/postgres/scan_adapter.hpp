// GENERATED BACKEND — engine-local scan helper (aux rebuild / contract)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {

// Engine-local base-table access using PostgreSQL table-AM (heap scan).
// NOT a portable DataChunk IR. Columns specify projection order.
//
// Each PostgresScanRow is one projected *cell* (row-major within a batch).
// `consumer` is invoked with `column_count` cells per logical row in each
// batch (or a partial final batch).
struct PostgresScanColumn {
  std::string name;
};

struct PostgresScanSpec {
  std::string schema_name;
  std::string table_name;
  std::vector<PostgresScanColumn> projected_columns;
  // Simple predicate: if filter_column is non-empty, applies
  // filter_op (0=EQ, 1=NE, 2=LT, 3=GT) against filter_value.
  std::string filter_column;
  int filter_op = 0;
  std::string filter_value;
  std::size_t batch_size = 1;
};

struct PostgresScanRow {
  bool is_null = true;
  std::vector<uint8_t> data; // UTF-8 text of the cell when !is_null
};

using PostgresScanBatchConsumer = std::function<void(
    const std::vector<PostgresScanRow> &cells, std::size_t column_count)>;

// Stream projected base-table rows via native PostgreSQL table-AM.
// Real implementation is compiled only into MODULE accelerator_codegen_pg
// (ACCELERATOR_CODEGEN_POSTGRES_EXTENSION). Host/stub builds fail closed.
void ScanPostgresTableForRebuild(const PostgresScanSpec &scan_spec,
                                 const PostgresScanBatchConsumer &consumer);

} // namespace accelerator_codegen::aux_state::engine::postgres
