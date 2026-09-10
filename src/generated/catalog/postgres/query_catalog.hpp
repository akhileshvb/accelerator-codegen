// GENERATED BACKEND — PostgreSQL query catalog.
// Empty until accelerator kernels are generated under src/generated/{bench}/q{N}/postgres/.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

struct PostgresSharedQueryArguments {
  std::vector<core::AuxValue> values;
};

struct PostgresSharedQueryCatalogEntry {
  int query_id = 0;
  const char *workload_name = nullptr;
  const char *accelerator_id = nullptr;
  const char *base_sql = nullptr;
  const char *accelerator_sql = nullptr;
  const char *aux_sql = nullptr;
};

const PostgresSharedQueryCatalogEntry &LookupPostgresQuery(const std::string &workload_name,
                                                            int query_id);

std::vector<int> PostgresQueriesForWorkload(const std::string &workload_name);

} // namespace accelerator_codegen::aux_state::engine::postgres
