// GENERATED BACKEND — engine-local scan helper for MySQL aux rebuild.
// Symbol: ScanMysqlTableForRebuild (from backend_analysis.json native_scan_plan).
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace accelerator_codegen::aux_state::engine::mysql {

struct MysqlScanColumn {
  std::string name;
};

struct MysqlScanFilter {
  std::size_t column_index = 0;
  std::string value;
};

struct MysqlScanSpec {
  std::string catalog_name;
  std::string schema_name;
  std::string table_name;
  std::vector<MysqlScanColumn> projected_columns;
  std::optional<MysqlScanFilter> filter;
};

// Engine-local row consumer: receives projected column values as strings.
// `col_vals` has `column_count` entries in projected-column order.
// The string "NULL" represents a SQL NULL.
using MysqlScanRowConsumer =
    std::function<void(const std::vector<std::string> &col_vals,
                       std::size_t column_count)>;

// Abstract interface for MySQL table scanning.
// Two implementations:
//   - InProcMysqlScanner (default, standalone tests — uses SeedMysqlTable)
//   - Real MySQL-native scanner (plugin — reads live MySQL tables)
class IMysqlTableScanner {
public:
  virtual ~IMysqlTableScanner() = default;
  virtual void Scan(const MysqlScanSpec &spec,
                    const MysqlScanRowConsumer &consumer) = 0;
};

// Scan a base table via native storage into typed callbacks.
// Forbidden: client-SQL round-trip on the hot path.
// Forbidden: converting into portable DataChunk IR.
// Delegates to the globally registered IMysqlTableScanner.
void ScanMysqlTableForRebuild(const MysqlScanSpec &spec,
                              const MysqlScanRowConsumer &consumer);

// Register a scanner implementation. Called once at init time.
// The default (when none registered) is the in-process mock.
void SetMysqlTableScanner(std::shared_ptr<IMysqlTableScanner> scanner);
std::shared_ptr<IMysqlTableScanner> GetMysqlTableScanner();

// -------- Test-only helpers (seed / clear in-process table registry) --------
// These operate on the built-in in-process mock, independent of the
// registered scanner. They are always available for standalone tests.
void SeedMysqlTable(const std::string &table_name,
                    const std::vector<std::string> &column_names,
                    const std::vector<std::vector<std::string>> &rows);

void ClearAllMysqlTables();

} // namespace accelerator_codegen::aux_state::engine::mysql
