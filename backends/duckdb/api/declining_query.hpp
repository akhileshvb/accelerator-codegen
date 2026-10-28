// GENERATED BACKEND
#pragma once

// A query registered with no accelerator behind it: the catalog entry (the query's SQL, its fallback) and the
// table function exist, and the module declines every read, so every call answers from the fallback SQL.
// The pipeline scaffolds a query's aux/register.cpp with one of these before the query's tests are written;
// the implementation replaces it with the real registration (aux_query_registry.hpp, catalog/query_catalog.hpp).

#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"
#include "substrate/aux/core/aux_query_module.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

struct DecliningQuery {
  int query_id = 0;
  std::string workload_name; // the benchmark, as its workload names it
  std::string accelerator_id;
  std::string table_function;
  std::vector<std::string> argument_types; // SQL type names of the table function's arguments
  std::string base_sql;                    // the query with its default arguments, or a $n template (no default binding)
  std::string fallback_sql;                // the query for any arguments: $n is the n-th argument
  std::vector<std::string> tables;         // the tables the query reads
  std::vector<std::string> binding;        // the default arguments, as text: registered like any module's
};

namespace declining_detail {

inline ::duckdb::LogicalType ArgumentType(const std::string &name) {
  if (name == "DATE") return ::duckdb::LogicalType::DATE;
  if (name == "INTEGER") return ::duckdb::LogicalType::INTEGER;
  if (name == "BIGINT") return ::duckdb::LogicalType::BIGINT;
  if (name == "DOUBLE" || name == "DECIMAL") return ::duckdb::LogicalType::DOUBLE;
  if (name == "VARCHAR") return ::duckdb::LogicalType::VARCHAR;
  throw std::runtime_error("declining query: unsupported argument type " + name);
}

// A default argument: its text cast to the argument's type.
inline core::AuxValue ArgumentValue(const std::string &type, const std::string &text) {
  const auto value = ::duckdb::Value(text).DefaultCastAs(ArgumentType(type));
  if (type == "DATE") return catalog_values::Date(catalog_values::DateDays(value));
  if (type == "INTEGER" || type == "BIGINT") return catalog_values::Int(value);
  if (type == "DOUBLE" || type == "DECIMAL") return catalog_values::Number(value);
  return catalog_values::Text(value);
}

// SELECT * FROM <table function>($1, ..., $n): the entry point a harness binds the arguments to.
inline std::string ParameterizedCall(const std::string &table_function, std::size_t arity) {
  std::string call = "SELECT * FROM " + table_function + "(";
  for (std::size_t i = 1; i <= arity; ++i) call += (i > 1 ? ", $" : "$") + std::to_string(i);
  return call + ")";
}

class NoTxnBuffer final : public core::AuxTxnBuffer {
public:
  bool HasPendingChanges() const override { return false; }
  void Clear() override {}
};

class DecliningModule final : public core::AuxQueryModule {
public:
  DecliningModule(core::AcceleratorId id, std::size_t tables) : id_(std::move(id)), tables_(tables) {}

  core::AcceleratorId Id() const override { return id_; }
  std::vector<core::TableDependency> Dependencies() const override {
    std::vector<core::TableDependency> dependencies;
    for (std::size_t table = 0; table < tables_; ++table) {
      core::TableDependency dependency;
      dependency.table = table;
      dependencies.push_back(std::move(dependency));
    }
    return dependencies;
  }
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override { return std::make_unique<NoTxnBuffer>(); }
  void Build(core::AuxEngineServices &, core::StateBuilder &builder) override { builder.Finish(); }
  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override { return {}; }
  void AccumulateMutation(const core::MutationBatch &, core::AuxEngineServices &, core::AuxTxnBuffer &) override {}
  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &, const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &) override {
    return {};
  }
  bool CheckValidity(core::AuxEngineServices &, const core::QueryArguments &) const override { return false; }
  void Execute(core::AuxEngineServices &services, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    ExecuteFallback(services, arguments, sink);
  }

private:
  core::AcceleratorId id_;
  std::size_t tables_;
};

struct DecliningBinding final : DuckdbRuntime::Binding {
  DecliningBinding(core::AcceleratorId id, CaptureTableList tables_p)
      : module(std::move(id), tables_p.size()), tables(std::move(tables_p)) {}
  core::AuxQueryModule &Module() override { return module; }
  void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) override { module.Build(backend, builder); }
  void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments, core::AuxRowSink &sink) override {
    module.Execute(backend, arguments, sink);
  }
  CaptureTableList CaptureTables() const override { return tables; }
  DecliningModule module;
  CaptureTableList tables;
};

// The catalog entry holds C strings: they live as long as the process.
inline core::AuxType ArgumentAuxType(const std::string &name) {
  if (name == "DATE") return core::AuxType::Date;
  if (name == "INTEGER" || name == "BIGINT") return core::AuxType::Int64;
  if (name == "DOUBLE" || name == "DECIMAL") return core::AuxType::Double;
  return core::AuxType::String;
}

// "fn($1, ..., $n)", the call of a table function that has no zero-argument form.

inline const char *Keep(std::string text) {
  static std::deque<std::string> kept;
  kept.push_back(std::move(text));
  return kept.back().c_str();
}

} // namespace declining_detail

inline void RegisterDecliningQuery(const DecliningQuery &query) {
  SharedQueryCatalogEntry entry;
  entry.query_id = query.query_id;
  entry.workload_name = declining_detail::Keep(query.workload_name);
  entry.accelerator_id = declining_detail::Keep(query.accelerator_id);
  entry.base_sql = declining_detail::Keep(query.base_sql);
  // With its default arguments the query registers as any module does (the parameterized call, the arguments in
  // the entry), so the harnesses are checked on the shape real modules have. A $n template without them has no
  // default binding: the parameterized call and the signature alone. Otherwise, a call of no arguments.
  const bool bound = !query.binding.empty() && query.binding.size() == query.argument_types.size();
  const bool parameterized = bound || query.base_sql.find('$') != std::string::npos;
  entry.duckdb_aux_sql = declining_detail::Keep(
      parameterized ? declining_detail::ParameterizedCall(query.table_function, query.argument_types.size())
                    : "SELECT * FROM " + query.table_function + "()");
  if (parameterized) {
    for (std::size_t i = 0; i < query.argument_types.size(); ++i) {
      const auto &type = query.argument_types[i];
      entry.benchmark_arguments.values.push_back(
          bound ? declining_detail::ArgumentValue(type, query.binding[i])
                : core::AuxValue::Null(declining_detail::ArgumentAuxType(type)));
    }
  }
  entry.fallback_sql = declining_detail::Keep(query.fallback_sql);
  RegisterSharedQuery(entry);

  AuxQuery aux;
  aux.accelerator_id = query.accelerator_id;
  aux.table_function = query.table_function;
  for (const auto &type : query.argument_types) {
    aux.argument_types.push_back(declining_detail::ArgumentType(type));
  }
  if (!parameterized) aux.overload_argument_types.push_back({});
  aux.make_binding = [query](::duckdb::Connection &) {
    CaptureTableList tables;
    for (std::size_t table = 0; table < query.tables.size(); ++table) {
      tables.emplace_back(query.tables[table], static_cast<core::TableId>(table));
    }
    return std::unique_ptr<DuckdbRuntime::Binding>(new declining_detail::DecliningBinding(query.accelerator_id, std::move(tables)));
  };
  RegisterAuxQuery(std::move(aux));
}

struct DecliningQueryRegistrar {
  explicit DecliningQueryRegistrar(const DecliningQuery &query) { RegisterDecliningQuery(query); }
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
