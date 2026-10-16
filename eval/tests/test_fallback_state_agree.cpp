// The SQL fallback answers exactly as the state does: each call of a SEC-EDGAR
// aux table function (default and parameterized) read from the state, then
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
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace {

namespace catalog = accelerator_codegen::aux_state::engine;

struct Calls {
  int query_id;
  std::vector<std::string> arguments; // SQL text of each call's argument list
};

const std::vector<Calls> kCalls = {
    {1, {""}},
    {2, {"", "2022, 'pure'", "2021, 'USD'"}},
    {3, {"", "2021, 'USD'", "2023, 'shares'", "2022, 'nonexistent'", "NULL, 'USD'"}},
    {4, {"", "1000, 3999, 'IS', 'USD'", "0, 9999, 'BS', 'shares'", "4000, 4999, 'EQ', NULL"}},
    {6, {"", "2022, 'BS', 'USD'", "2021, 'CF', 'shares'", "2023, 'IS', 'nonexistent'"}},
    {24, {"", "2022, 'USD'", "2021, 'pure'", "2147483647, 'USD'", "NULL, 'USD'"}},
};

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
  for (const auto &group : kCalls) {
    const auto &entry = catalog::LookupSharedQuery("sec_edgar", group.query_id);
    catalog::duckdb::DuckdbRuntime runtime(connection, entry, 10.0);
    runtime.Build();
    for (const auto &arguments : group.arguments) {
      const auto sql = "SELECT * FROM accel_sec_edgar_q" + std::to_string(group.query_id) + "_aux(" + arguments + ")";
      unsetenv("ACCEL_AUX_FORCE_FALLBACK");
      auto from_state = Read(connection, sql);
      setenv("ACCEL_AUX_FORCE_FALLBACK", "1", 1);
      auto from_sql = Read(connection, sql);
      unsetenv("ACCEL_AUX_FORCE_FALLBACK");
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
