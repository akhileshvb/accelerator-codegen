// SCAFFOLDING
#include "eval/harness/param_sets.hpp"

#include <stdexcept>

#include "backends/duckdb/api/shared_query_engine.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "eval/harness/param_source.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {
namespace {

// DuckDB reads the literal list, so its values have the types SQL gives the literals.
core::QueryArguments ArgumentsOfLiterals(duckdb::Connection &connection, const aux_engine::SharedQueryCatalogEntry &entry,
                                         const std::string &literals) {
  auto result = connection.Query("SELECT " + literals);
  if (!result || result->HasError() || result->RowCount() != 1) {
    throw std::runtime_error("parameter literals (" + literals + "): " + (result ? result->GetError() : "no result"));
  }
  std::vector<duckdb::Value> values;
  for (duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
    values.push_back(result->GetValue(column, 0));
  }
  if (values.size() != entry.benchmark_arguments.values.size()) {
    throw std::runtime_error("parameter literals (" + literals + ") do not match the signature of " +
                             entry.accelerator_id);
  }
  return aux_engine::AuxArgumentsFromValues(entry, values);
}

} // namespace

std::vector<core::QueryArguments> ArgumentSets(const aux_engine::SharedQueryCatalogEntry &entry) {
  if (aux_engine::HasDefaultBinding(entry)) {
    return {aux_engine::DefaultArguments(entry)};
  }
  duckdb::DuckDB database(nullptr);
  duckdb::Connection connection(database);
  std::vector<core::QueryArguments> sets;
  for (const auto &literals : DrawArgumentLists(entry.workload_name, entry.query_id)) {
    sets.push_back(ArgumentsOfLiterals(connection, entry, literals));
  }
  return sets;
}

std::string ArgumentLiterals(const core::QueryArguments &arguments) {
  std::string out;
  for (const auto &argument : arguments.values) {
    out += (out.empty() ? "" : ", ") + aux_engine::AuxValueToDuckdb(argument).ToSQLString();
  }
  return out;
}

} // namespace accelerator_codegen::aux_state::harness
