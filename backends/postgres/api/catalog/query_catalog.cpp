// GENERATED BACKEND — PostgreSQL query catalog registry (no query is named here).
#include "backends/postgres/api/catalog/query_catalog.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace {

// Keyed by (workload, query id): ordered by query id, node-stable so
// LookupPostgresQuery can return references.
std::map<std::pair<std::string, int>, PostgresSharedQueryCatalogEntry> &Registry() {
  static std::map<std::pair<std::string, int>, PostgresSharedQueryCatalogEntry> registry;
  return registry;
}

} // namespace

void RegisterPostgresQuery(const PostgresSharedQueryCatalogEntry &entry) {
  Registry()[{entry.workload_name, entry.query_id}] = entry;
}

const PostgresSharedQueryCatalogEntry &LookupPostgresQuery(const std::string &workload_name,
                                                             int query_id) {
  const auto it = Registry().find({workload_name, query_id});
  if (it == Registry().end()) {
    throw std::runtime_error(
        "postgres query not found: " + workload_name + " Q" + std::to_string(query_id));
  }
  return it->second;
}

std::vector<int> PostgresQueriesForWorkload(const std::string &workload_name) {
  std::vector<int> ids;
  for (const auto &[key, entry] : Registry()) {
    if (key.first == workload_name) ids.push_back(key.second);
  }
  return ids;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
