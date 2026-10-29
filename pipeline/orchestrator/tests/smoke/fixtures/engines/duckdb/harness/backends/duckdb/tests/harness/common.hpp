#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/tests/test_support/result_compare.hpp"

#ifndef ACCEL_BENCHMARK_NAME
#define ACCEL_BENCHMARK_NAME ""
#endif
#ifndef ACCEL_SOURCE_DIR
#define ACCEL_SOURCE_DIR "."
#endif

namespace accelerator_codegen::duckdb_harness {

namespace engine = accelerator_codegen::aux_state::engine;
namespace duck_engine = accelerator_codegen::aux_state::engine::duckdb;
namespace fs = std::filesystem;
using Entry = engine::SharedQueryCatalogEntry;

inline std::string Benchmark() {
  if (std::string(ACCEL_BENCHMARK_NAME).size()) return ACCEL_BENCHMARK_NAME;
  const char *env = std::getenv("ACCEL_AUX_BENCHMARK");
  return env ? env : "";
}

inline std::string DbPath() {
  const char *path = std::getenv("ACCEL_DB");
  if (!path || !*path) throw std::runtime_error("ACCEL_DB is not set; run through host.sh");
  return path;
}

inline fs::path TempDirectory() {
  const char *path = std::getenv("TMPDIR");
  const fs::path directory = path && *path ? fs::path(path) : fs::temp_directory_path();
  fs::create_directories(directory);
  return directory;
}

inline const Entry &Lookup(int query, const std::string &benchmark = Benchmark()) {
  const Entry *registered = nullptr;
  for (const auto *aux : duck_engine::AuxQueries()) {
    if (!aux) continue;
    const auto *candidate = engine::FindSharedQueryByAccelerator(aux->accelerator_id);
    if (!candidate || candidate->query_id != query || !candidate->workload_name) continue;
    if (!benchmark.empty() && benchmark != candidate->workload_name) continue;
    if (engine::IsParallelAuxAccelerator(aux->accelerator_id.c_str())) continue;
    if (registered && registered->accelerator_id && candidate->accelerator_id &&
        std::string(registered->accelerator_id) != candidate->accelerator_id) {
      throw std::runtime_error("multiple registered accelerators match query " +
                               std::to_string(query) +
                               "; set ACCEL_AUX_BENCHMARK or use a query-specific target");
    }
    registered = candidate;
  }
  if (registered) return *registered;
  if (!benchmark.empty()) return engine::LookupSharedQuery(benchmark, query);
  const fs::path root = fs::path(ACCEL_SOURCE_DIR) / "workloads";
  const Entry *match = nullptr;
  for (const auto &dir : fs::directory_iterator(root)) {
    if (!dir.is_directory()) continue;
    try {
      const auto &candidate = engine::LookupSharedQuery(dir.path().filename().string(), query);
      if (match) throw std::runtime_error("query number is ambiguous; set ACCEL_AUX_BENCHMARK");
      match = &candidate;
    } catch (const std::runtime_error &error) {
      const std::string message = error.what();
      if (message.find("ambiguous") != std::string::npos) throw;
    }
  }
  if (!match) throw std::runtime_error("no registered query " + std::to_string(query));
  return *match;
}

inline std::size_t SqlParameterCount(const char *sql) {
  if (!sql) return 0;
  const std::string text(sql);
  std::size_t maximum = 0;
  bool quoted = false;
  for (std::size_t i = 0; i < text.size();) {
    if (!quoted && i + 1 < text.size() && text[i] == '-' && text[i + 1] == '-') {
      i = text.find('\n', i + 2);
      if (i == std::string::npos) break;
      continue;
    }
    if (!quoted && i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
      const auto end = text.find("*/", i + 2);
      if (end == std::string::npos) break;
      i = end + 2;
      continue;
    }
    if (text[i] == '\'') {
      if (quoted && i + 1 < text.size() && text[i + 1] == '\'') {
        i += 2;
        continue;
      }
      quoted = !quoted;
      ++i;
      continue;
    }
    if (!quoted && text[i] == '$' && i + 1 < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
      std::size_t end = i + 1;
      while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
      maximum = std::max(maximum, static_cast<std::size_t>(std::stoul(text.substr(i + 1, end - i - 1))));
      i = end;
      continue;
    }
    ++i;
  }
  return maximum;
}

inline std::unique_ptr<duckdb::DuckDB> OpenDatabase(const std::string &path = DbPath()) {
  duckdb::DBConfig config;
  const char *memory_mb = std::getenv("ACCEL_ENGINE_MEMORY_MB");
  if (memory_mb && *memory_mb) {
    const auto megabytes = std::stoull(memory_mb);
    const auto max_megabytes = std::numeric_limits<duckdb::idx_t>::max() / (1024ULL * 1024ULL);
    if (megabytes == 0 || megabytes > max_megabytes)
      throw std::runtime_error("ACCEL_ENGINE_MEMORY_MB must be a positive DuckDB memory limit in MB");
    config.options.maximum_memory = static_cast<duckdb::idx_t>(megabytes * 1024ULL * 1024ULL);
  }
  return std::make_unique<duckdb::DuckDB>(path, &config);
}

inline std::unique_ptr<duckdb::Connection> Connect(duckdb::DuckDB &database) {
  auto connection = std::make_unique<duckdb::Connection>(database);
  static std::mutex registration_mutex;
  static std::unordered_set<duckdb::DuckDB *> registered;
  {
    std::lock_guard<std::mutex> lock(registration_mutex);
    if (registered.insert(&database).second) accelerator_codegen::RegisterAcceleratorFunctions(*connection);
  }
  return connection;
}

inline void Require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}

inline std::unique_ptr<duckdb::MaterializedQueryResult> Query(duckdb::Connection &connection,
                                                              const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result && !result->HasError(), "SQL failed: " + (result ? result->GetError() : "null result") +
                                             "\n" + sql);
  return result;
}

inline void Exec(duckdb::Connection &connection, const std::string &sql) {
  Query(connection, sql);
}

inline std::string ReadFile(const fs::path &path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

inline std::vector<std::string> SplitTuple(const std::string &line) {
  std::vector<std::string> values;
  bool quoted = false;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '\'') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '\'') {
        ++i;
      } else {
        quoted = !quoted;
      }
    } else if (!quoted && ch == '(') {
      ++depth;
    } else if (!quoted && ch == ')') {
      --depth;
    } else if (!quoted && depth == 0 && ch == ',') {
      values.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < line.size() || !line.empty()) values.push_back(line.substr(start));
  for (auto &value : values) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
  }
  return values;
}

inline std::string BindSql(const std::string &sql, const std::vector<std::string> &values) {
  std::string out;
  bool quoted = false;
  for (std::size_t i = 0; i < sql.size();) {
    if (sql[i] == '\'') {
      out.push_back(sql[i]);
      if (quoted && i + 1 < sql.size() && sql[i + 1] == '\'') {
        out.push_back(sql[++i]);
      } else {
        quoted = !quoted;
      }
      ++i;
    } else if (!quoted && sql[i] == '$' && i + 1 < sql.size() && std::isdigit(sql[i + 1])) {
      std::size_t end = i + 1;
      while (end < sql.size() && std::isdigit(sql[end])) ++end;
      const auto index = static_cast<std::size_t>(std::stoul(sql.substr(i + 1, end - i - 1)));
      Require(index > 0 && index <= values.size(), "missing SQL argument $" + std::to_string(index));
      out += "(" + values[index - 1] + ")";
      i = end;
    } else {
      out.push_back(sql[i++]);
    }
  }
  return out;
}

inline std::vector<std::string> CatalogArgumentLiterals(const Entry &entry) {
  std::vector<std::string> values;
  values.reserve(entry.benchmark_arguments.values.size());
  for (const auto &argument : entry.benchmark_arguments.values)
    values.push_back(engine::AuxValueToDuckdb(argument).ToSQLString());
  return values;
}

inline std::string AcceleratorSql(const Entry &entry, const std::vector<std::string> &values = {}) {
  const auto arguments = values.empty() ? CatalogArgumentLiterals(entry) : values;
  if (arguments.empty()) return entry.duckdb_aux_sql ? entry.duckdb_aux_sql : "";
  const auto *aux = duck_engine::FindAuxQuery(entry.accelerator_id ? entry.accelerator_id : "");
  Require(aux != nullptr, "registered accelerator has no DuckDB SQL function");
  std::ostringstream sql;
  sql << "SELECT * FROM " << aux->table_function << '(';
  for (std::size_t i = 0; i < arguments.size(); ++i) {  // an expression's own type (a date plus an interval) is cast to the parameter's
    sql << (i ? ", " : "");
    if (i < aux->argument_types.size())
      sql << "CAST(" << arguments[i] << " AS " << aux->argument_types[i].ToString() << ')';
    else
      sql << arguments[i];
  }
  sql << ')';
  return sql.str();
}

inline std::pair<std::string, std::string> SqlPair(const Entry &entry,
                                                   const std::vector<std::string> &values = {}) {
  const auto arguments = values.empty() ? CatalogArgumentLiterals(entry) : values;
  const std::string reference = arguments.empty() || !entry.fallback_sql
                                    ? (entry.base_sql ? entry.base_sql : "")
                                    : BindSql(entry.fallback_sql, arguments);
  return {reference, AcceleratorSql(entry, arguments)};
}

inline std::vector<std::vector<std::string>> SuiteTuples(const int query) {
  const char *suite_env = std::getenv("ACCEL_SUITE_DIR");
  if (!suite_env || !*suite_env) return {};
  fs::path params = fs::path(suite_env) / "params.sql";
  std::vector<std::vector<std::string>> tuples;
  std::istringstream input(ReadFile(params));
  for (std::string line; std::getline(input, line);) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '-') continue;
    auto tuple = SplitTuple(line);
    if (!tuple.empty()) tuples.push_back(std::move(tuple));
  }
  const char *extra = std::getenv("ACCEL_SUITE_PARAMS_FILE");
  if (extra && *extra) {
    std::istringstream additions(ReadFile(extra));
    for (std::string line; std::getline(additions, line);) {
      const auto first = line.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || line[first] == '-') continue;
      auto tuple = SplitTuple(line);
      if (!tuple.empty()) tuples.push_back(std::move(tuple));
    }
  }
  (void)query;
  return tuples;
}

inline std::size_t ArgumentCount(const Entry &entry) {
  const auto *aux = duck_engine::FindAuxQuery(
      entry.accelerator_id ? entry.accelerator_id : "");
  const auto registered_count =
      std::max({aux ? aux->argument_types.size() : std::size_t{0},
                entry.benchmark_arguments.values.size(),
                SqlParameterCount(entry.fallback_sql)});
  if (registered_count != 0) return registered_count;

  auto tuples = SuiteTuples(entry.query_id);
  if (!tuples.empty()) return tuples.front().size();
  if (!entry.workload_name) return 0;
  const fs::path params = fs::path(ACCEL_SOURCE_DIR) / "accelerators" /
                          entry.workload_name /
                          ("q" + std::to_string(entry.query_id)) /
                          "tests" / "workload" / "params.sql";
  if (!fs::exists(params)) return 0;
  std::istringstream input(ReadFile(params));
  for (std::string line; std::getline(input, line);) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '-') continue;
    const auto tuple = SplitTuple(line);
    if (!tuple.empty()) return tuple.size();
  }
  return 0;
}

inline std::vector<std::vector<std::string>> WorkloadSuiteTuples(
    const std::string &benchmark, int query, std::size_t argument_count) {
  auto tuples = SuiteTuples(query);
  const fs::path workload_suite =
      fs::path(ACCEL_SOURCE_DIR) / "accelerators" / benchmark /
      ("q" + std::to_string(query)) / "tests" / "workload" / "params.sql";
  if (tuples.empty() && fs::exists(workload_suite)) {
    std::istringstream input(ReadFile(workload_suite));
    for (std::string line; std::getline(input, line);) {
      const auto first = line.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || line[first] == '-') continue;
      auto tuple = SplitTuple(line);
      if (!tuple.empty()) tuples.push_back(std::move(tuple));
    }
  }
  if (!tuples.empty() && argument_count == 0) argument_count = tuples.front().size();
  if (tuples.empty() && argument_count == 0) tuples.emplace_back();
  Require(!tuples.empty(),
          "no workload parameter tuples found for " + benchmark + " query " +
              std::to_string(query));
  for (const auto &tuple : tuples) {
    Require(tuple.size() == argument_count,
            "workload parameter tuple has " + std::to_string(tuple.size()) +
                " values; query expects " + std::to_string(argument_count));
  }
  return tuples;
}

inline void Compare(duckdb::Connection &connection, const Entry &entry,
                    const std::vector<std::string> &values = {}, const std::string &label = "differential") {
  const auto sql = SqlPair(entry, values);
  Require(!sql.first.empty() && !sql.second.empty(), "catalog SQL is incomplete");
  if (entry.compare_unordered) {
    auto expected = Query(connection, sql.first);
    auto actual = Query(connection, sql.second);
    Require(expected->ColumnCount() == actual->ColumnCount() && expected->RowCount() == actual->RowCount(),
            label + ": result shape differs");
    std::vector<std::string> left, right;
    for (duckdb::idx_t row = 0; row < expected->RowCount(); ++row) {
      std::string a, b;
      for (duckdb::idx_t col = 0; col < expected->ColumnCount(); ++col) {
        a += expected->GetValue(col, row).ToString() + '\x1f';
        b += actual->GetValue(col, row).ToString() + '\x1f';
      }
      left.push_back(a);
      right.push_back(b);
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    Require(left == right, label + ": unordered result differs");
  } else {
    test_support::RequireResultsEqualOrdered(connection, label, sql.first, sql.second);
  }
}

inline std::vector<fs::path> SuiteTransactions() {
  const char *suite_env = std::getenv("ACCEL_SUITE_DIR");
  if (!suite_env || !*suite_env) return {};
  fs::path txns = fs::path(suite_env) / "txns";
  std::vector<fs::path> paths;
  if (!fs::exists(txns)) return paths;
  for (const auto &file : fs::directory_iterator(txns))
    if (file.is_regular_file() && file.path().extension() == ".sql") paths.push_back(file.path());
  std::sort(paths.begin(), paths.end());
  const char *selected = std::getenv("ACCEL_SUITE_TXNS");
  if (selected && *selected) {
    std::vector<fs::path> filtered;
    std::istringstream names(selected);
    for (std::string name; std::getline(names, name, ',');)
      for (const auto &path : paths)
        if (path.filename() == name) filtered.push_back(path);
    return filtered;
  }
  return paths;
}

inline int QueryArgument(int argc, char **argv, const char *usage) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << usage << '\n';
      return 0;
    }
    if (arg == "--query" && i + 1 < argc) return std::stoi(argv[i + 1]);
    if (!arg.empty() && std::isdigit(arg[0])) return std::stoi(arg);
  }
  throw std::invalid_argument(std::string("Usage: ") + usage);
}

} // namespace accelerator_codegen::duckdb_harness
