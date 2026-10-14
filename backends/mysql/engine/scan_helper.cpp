// GENERATED BACKEND — engine-local scan helper implementation.
// Uses abstract IMysqlTableScanner to avoid ODR conflicts between
// standalone lib and MySQL plugin. Default scanner = in-process mock.
#include "backends/mysql/engine/scan_helper.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace accelerator_codegen::aux_state::engine::mysql {

// ========== In-process mock scanner (fallback / standalone tests) ==========

namespace {

struct InProcTable {
  std::vector<std::string> column_names;
  std::vector<std::vector<std::string>> rows;
};

std::mutex g_tables_mutex;
std::unordered_map<std::string, InProcTable> g_tables;

bool MatchFilter(const std::vector<std::string> &row,
                 const std::vector<std::string> &column_names,
                 const MysqlScanFilter &filter,
                 const std::vector<MysqlScanColumn> &projected) {
  if (filter.column_index >= projected.size()) return true;
  const auto &col = projected[filter.column_index];

  auto it = std::find(column_names.begin(), column_names.end(), col.name);
  if (it == column_names.end()) return false;
  std::size_t base_idx = static_cast<std::size_t>(
      std::distance(column_names.begin(), it));
  if (base_idx >= row.size()) return false;

  return row[base_idx] == filter.value;
}

// ========== Default in-process scanner ==========

class InProcMysqlScanner : public IMysqlTableScanner {
public:
  void Scan(const MysqlScanSpec &spec,
            const MysqlScanRowConsumer &consumer) override {
    std::lock_guard<std::mutex> lock(g_tables_mutex);

    auto it = g_tables.find(spec.table_name);
    if (it == g_tables.end()) {
      return;
    }

    const auto &table = it->second;

    // Build projection: for each projected column, find its base index.
    std::vector<std::size_t> base_indices;
    base_indices.reserve(spec.projected_columns.size());
    for (const auto &pc : spec.projected_columns) {
      auto col_it = std::find(table.column_names.begin(),
                              table.column_names.end(), pc.name);
      if (col_it == table.column_names.end()) {
        base_indices.push_back(static_cast<std::size_t>(-1));
      } else {
        base_indices.push_back(static_cast<std::size_t>(
            std::distance(table.column_names.begin(), col_it)));
      }
    }

    for (const auto &row : table.rows) {
      // Apply filter if present.
      if (spec.filter.has_value()) {
        if (!MatchFilter(row, table.column_names, *spec.filter,
                         spec.projected_columns)) {
          continue;
        }
      }

      // Project the columns.
      std::vector<std::string> projected_vals;
      projected_vals.reserve(spec.projected_columns.size());
      for (std::size_t i = 0; i < spec.projected_columns.size(); ++i) {
        std::size_t bi = base_indices[i];
        if (bi == static_cast<std::size_t>(-1) || bi >= row.size()) {
          projected_vals.push_back("NULL");
        } else {
          projected_vals.push_back(row[bi]);
        }
      }
      consumer(projected_vals, projected_vals.size());
    }
  }
};

// ========== Global scanner registry ==========

std::mutex g_scanner_mutex;
std::shared_ptr<IMysqlTableScanner> g_scanner;

std::shared_ptr<IMysqlTableScanner> GetOrCreateDefaultScanner() {
  // Double-check: create default if none registered.
  if (!g_scanner) {
    g_scanner = std::make_shared<InProcMysqlScanner>();
  }
  return g_scanner;
}

} // namespace

void SeedMysqlTable(const std::string &table_name,
                    const std::vector<std::string> &column_names,
                    const std::vector<std::vector<std::string>> &rows) {
  std::lock_guard<std::mutex> lock(g_tables_mutex);
  InProcTable t;
  t.column_names = column_names;
  t.rows = rows;
  g_tables[table_name] = std::move(t);
}

void ClearAllMysqlTables() {
  std::lock_guard<std::mutex> lock(g_tables_mutex);
  g_tables.clear();
}

void SetMysqlTableScanner(std::shared_ptr<IMysqlTableScanner> scanner) {
  std::lock_guard<std::mutex> lock(g_scanner_mutex);
  g_scanner = std::move(scanner);
}

std::shared_ptr<IMysqlTableScanner> GetMysqlTableScanner() {
  std::lock_guard<std::mutex> lock(g_scanner_mutex);
  if (!g_scanner) {
    g_scanner = std::make_shared<InProcMysqlScanner>();
  }
  return g_scanner;
}

void ScanMysqlTableForRebuild(const MysqlScanSpec &spec,
                              const MysqlScanRowConsumer &consumer) {
  GetMysqlTableScanner()->Scan(spec, consumer);
}

} // namespace accelerator_codegen::aux_state::engine::mysql
