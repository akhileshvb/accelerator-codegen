// GENERATED BACKEND
#pragma once

// The DuckDB backend's extension point for aux-state accelerators.
//
// The backend names no query.  Each query's directory
// (accelerators/<bench>/q<N>/duckdb/aux/register.cpp) registers its
// accelerators here with a static AuxQueryRegistrar: the table function
// (accel_<...>), its SQL argument types, and how to bind the query's module
// to a connection (the module plus the query's InitialScan implementation).
// The catalog entry with the same accelerator id is registered next to it
// (catalog/query_catalog.hpp, SharedQueryRegistrar).
//
// The scan helpers (engine/scan.hpp, included here) are the backend's
// engine services for those InitialScan implementations (SQL on the
// runtime's connection, value decoding).  They are engine-level, not per
// query.

#include <mutex>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// (DuckDB table name, the module's TableId for it): the tables engine capture
// watches for the module.
using CaptureTableList = std::vector<std::pair<std::string, core::TableId>>;

struct AuxQuery {
  // The catalog entry's accelerator id (FindSharedQueryByAccelerator).
  std::string accelerator_id;
  // The SQL table function serving it, e.g. accel_<bench>_q<N>_aux.
  std::string table_function;
  std::vector<::duckdb::LogicalType> argument_types;
  std::function<std::unique_ptr<DuckdbRuntime::Binding>(::duckdb::Connection &)> make_binding;
  // Further SQL signatures of the same table function (overloads), e.g. a
  // parameterless form next to `argument_types`.
  std::vector<std::vector<::duckdb::LogicalType>> overload_argument_types;
};

void RegisterAuxQuery(AuxQuery query);
// In registration order.
std::vector<const AuxQuery *> AuxQueries();
const AuxQuery *FindAuxQuery(const std::string &accelerator_id);

struct AuxQueryRegistrar {
  explicit AuxQueryRegistrar(AuxQuery query) { RegisterAuxQuery(std::move(query)); }
};

// ---- binding a module and its scan to a connection ----

template <typename QueryModule>
struct ModuleBinding : DuckdbRuntime::Binding {
  QueryModule module;

  template <typename... Args>
  ModuleBinding(Args &&...args) : module(std::forward<Args>(args)...) {}

  core::AuxQueryModule &Module() override { return module; }
  void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) override {
    const scan::ScanServicesScope scans(backend);
    module.Build(backend, builder);
  }
  void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    const scan::ScanServicesScope scans(backend);
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
