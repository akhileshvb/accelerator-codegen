// The SQL fallback answers exactly as the state does: each call of a SEC-EDGAR
// aux table function (the drawn parameter sets and edge cases) read from the state, then
// again with ACCEL_AUX_FORCE_FALLBACK=1 (rows compared up to ORDER BY ties).
//   test_fallback_state_agree <sec_edgar.duckdb>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "eval/harness/param_sets.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;
namespace harness = accelerator_codegen::aux_state::harness;

// Edge-case argument lists (SQL text) read besides the parameter sets drawn for each query.
struct EdgeCalls {
  int query_id;
  std::vector<std::string> arguments;
};

const std::vector<EdgeCalls> kEdgeCalls = {
    {1, {"'nonexistent'", "NULL"}},
    {3, {"2022, 'nonexistent'", "NULL, 'USD'"}},
    {4, {"0, 9999, 'nonexistent', 'USD'", "NULL, 9999, 'BS', 'USD'", "0, 9999, 'BS', NULL"}},
    {6, {"2023, 'IS', 'nonexistent'"}},
    {24, {"2147483647, 'USD'", "NULL, 'USD'"}},
};

std::vector<std::string> CallsOf(const catalog::SharedQueryCatalogEntry &entry) {
  std::vector<std::string> calls;
  for (const auto &arguments : harness::ArgumentSets(entry)) {
    calls.push_back(harness::ArgumentLiterals(arguments));
  }
  for (const auto &edge : kEdgeCalls) {
    if (edge.query_id == entry.query_id) {
      calls.insert(calls.end(), edge.arguments.begin(), edge.arguments.end());
    }
  }
  return calls;
}

std::vector<std::vector<duckdb::Value>> Read(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null"));
  }
  std::vector<std::vector<duckdb::Value>> rows;
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    rows.emplace_back();
    for (duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      rows.back().push_back(result->GetValue(column, row));
    }
  }
  return rows;
}

// Rows tie on the query's ORDER BY: compare them by their exact columns.
void SortByExactColumns(std::vector<std::vector<duckdb::Value>> &rows) {
  const auto key = [](const std::vector<duckdb::Value> &row) {
    std::string out;
    for (const auto &value : row) {
      if (value.type().id() != duckdb::LogicalTypeId::DOUBLE) {
        out += value.ToString() + '\x1f';
      }
    }
    return out;
  };
  std::stable_sort(rows.begin(), rows.end(),
                   [&](const auto &a, const auto &b) { return key(a) < key(b); });
}

bool Same(const duckdb::Value &a, const duckdb::Value &b) {
  if (a.IsNull() || b.IsNull()) {
    return a.IsNull() == b.IsNull();
  }
  if (a.type().id() == duckdb::LogicalTypeId::DOUBLE) {
    const double x = a.GetValue<double>(), y = b.GetValue<double>();
    return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::fabs(y));
  }
  return a.ToString() == b.ToString();
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_fallback_state_agree <sec_edgar.duckdb>\n";
    return 2;
  }
  duckdb::DuckDB database(argv[1]);
  duckdb::Connection connection(database);
  accelerator_codegen::RegisterAcceleratorFunctions(connection);
  int failed = 0, checked = 0;
  for (const int query_id : catalog::SharedQueriesForWorkload("sec_edgar")) {
    const auto &entry = catalog::LookupSharedQuery("sec_edgar", query_id);
    catalog::duckdb::DuckdbRuntime runtime(connection, entry, 10.0);
    runtime.Build();
    for (const auto &arguments : CallsOf(entry)) {
      const auto sql = "SELECT * FROM accel_sec_edgar_q" + std::to_string(query_id) + "_aux(" + arguments + ")";
      unsetenv("ACCEL_AUX_FORCE_FALLBACK");
      std::vector<std::vector<duckdb::Value>> from_state, from_sql;
      try {
        from_state = Read(connection, sql);
        setenv("ACCEL_AUX_FORCE_FALLBACK", "1", 1);
        from_sql = Read(connection, sql);
        unsetenv("ACCEL_AUX_FORCE_FALLBACK");
      } catch (const std::exception &error) {
        unsetenv("ACCEL_AUX_FORCE_FALLBACK");
        ++checked;
        ++failed;
        std::cout << "ERROR " << sql << ": " << std::string(error.what()).substr(0, 200) << "\n";
        continue;
      }
      SortByExactColumns(from_state);
      SortByExactColumns(from_sql);
      bool ok = from_state.size() == from_sql.size();
      for (std::size_t i = 0; ok && i < from_state.size(); ++i) {
        for (std::size_t c = 0; ok && c < from_state[i].size(); ++c) {
          ok = Same(from_state[i][c], from_sql[i][c]);
          if (!ok) {
            std::cout << "row " << i << " column " << c << ": state " << from_state[i][c].ToString()
                      << " sql " << from_sql[i][c].ToString() << "\n";
          }
        }
      }
      ++checked;
      if (!ok) {
        ++failed;
        std::cout << "MISMATCH " << sql << " state rows=" << from_state.size() << " sql rows=" << from_sql.size() << "\n";
      }
    }
  }
  std::cout << "checked=" << checked << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 1;
}
