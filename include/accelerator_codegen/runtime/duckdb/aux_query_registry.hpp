// GENERATED BACKEND
#pragma once

// The DuckDB backend's extension point for aux-state accelerators.
//
// The backend names no query.  Each query's directory
// (src/generated/<bench>/q<N>/duckdb/aux/register.cpp) registers its
// accelerators here with a static AuxQueryRegistrar: the table function
// (accel_<...>), its SQL argument types, and how to bind the query's module
// to a connection (the module plus the query's InitialScan implementation).
// The catalog entry with the same accelerator id is registered next to it
// (catalog/query_catalog.hpp, SharedQueryRegistrar).
//
// The scan helpers below are the backend's engine services for those
// InitialScan implementations (SQL on the runtime's connection, value
// decoding).  They are engine-level, not per query.

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// (DuckDB table name, the module's TableId for it): the tables engine capture
// watches for the module.
using CaptureTableList = std::vector<std::pair<std::string, core::TableId>>;

struct AuxQuery {
  // The catalog entry's accelerator id (FindSharedQueryByAccelerator).
  std::string accelerator_id;
  // The SQL table function serving it, e.g. accel_tpch_q1_aux.
  std::string table_function;
  std::vector<::duckdb::LogicalType> argument_types;
  std::function<std::unique_ptr<DuckdbRuntime::Binding>(::duckdb::Connection &)> make_binding;
};

void RegisterAuxQuery(AuxQuery query);
// In registration order.
std::vector<const AuxQuery *> AuxQueries();
const AuxQuery *FindAuxQuery(const std::string &accelerator_id);

struct AuxQueryRegistrar {
  explicit AuxQueryRegistrar(AuxQuery query) { RegisterAuxQuery(std::move(query)); }
};

// ---- scan helpers for InitialScan implementations ----
namespace scan {

void Require(bool condition, const std::string &message);
std::string SqlDateLiteral(int32_t days);
std::string SqlDateInPredicate(const char *column, const std::set<int32_t> &days);
std::string SqlIntInPredicate(const char *column, const std::set<int32_t> &values);
std::string SqlInt64InPredicate(const char *column, const std::unordered_set<int64_t> &values);
// A DECIMAL's value at scale 2 (the aux kernels' fixed point).
int64_t DecimalRaw(const ::duckdb::Value &value);
int64_t SafeInt64(const ::duckdb::Value &value);
int32_t SafeInt32(const ::duckdb::Value &value);
int32_t DateDays(const ::duckdb::Value &value);
char FirstChar(const ::duckdb::Value &value);
// " LIMIT <n>" when the environment variable is set (bench scans).
std::string OptionalBenchScanLimitSuffix(const char *env_name);

// The connection a scan runs on: the runtime's, or a reader thread's private
// one (counted as a table scan either way).
::duckdb::Connection &ScanConnection(::duckdb::Connection &connection);

inline ::duckdb::Value ChunkValue(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t col,
                                  ::duckdb::idx_t row) {
  return chunk.GetValue(col, row);
}

template <typename Callback>
void ScanQuery(::duckdb::Connection &connection_p, const std::string &sql, Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  for (::duckdb::idx_t row_index = 0; row_index < result->RowCount(); ++row_index) {
    callback(*result, row_index);
  }
}

// Streaming scan for large SF builds: avoids materializing the full result.
template <typename Callback>
void ScanQueryStreaming(::duckdb::Connection &connection_p, const std::string &sql, Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : "DuckDB scan returned null");
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      break;
    }
    for (::duckdb::idx_t row_index = 0; row_index < chunk->size(); ++row_index) {
      callback(*chunk, row_index);
    }
  }
  Require(!result->HasError(), result->GetError());
}

} // namespace scan

// ---- binding a module and its scan to a connection ----

template <typename QueryModule>
struct ModuleBinding : DuckdbRuntime::Binding {
  QueryModule module;

  template <typename... Args>
  ModuleBinding(Args &&...args) : module(std::forward<Args>(args)...) {}

  core::AuxQueryModule &Module() override { return module; }
  void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) override { module.Build(backend, builder); }
  void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    module.Execute(backend, arguments, sink);
  }
};

// The module constructed from (config..., scan*), with the scan constructed
// from the connection.
template <typename Scan, typename QueryModule>
struct QueryBound : Scan, ModuleBinding<QueryModule> {
  template <typename... Args>
  QueryBound(::duckdb::Connection &connection, CaptureTableList tables_p, Args &&...args)
      : Scan(connection), ModuleBinding<QueryModule>(std::forward<Args>(args)..., static_cast<Scan *>(this)),
        tables(std::move(tables_p)) {}
  CaptureTableList CaptureTables() const override { return tables; }
  CaptureTableList tables;
};

template <typename Scan, typename QueryModule, typename... ConfigArgs>
std::unique_ptr<DuckdbRuntime::Binding> MakeBinding(::duckdb::Connection &connection, CaptureTableList tables,
                                                    ConfigArgs &&...config_args) {
  return std::unique_ptr<DuckdbRuntime::Binding>(
      new QueryBound<Scan, QueryModule>(connection, std::move(tables), std::forward<ConfigArgs>(config_args)...));
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
