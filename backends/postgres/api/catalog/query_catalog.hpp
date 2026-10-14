// GENERATED BACKEND — PostgreSQL query catalog registry.
// Names no query: each query registers its entry from
// src/generated/{bench}/q{N}/postgres/aux/catalog.cpp (PostgresQueryCatalogRegistrar).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"

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

// Plain function (defined in query_catalog.cpp, function-local storage); safe
// to call from static initialisers of any translation unit.
void RegisterPostgresQuery(const PostgresSharedQueryCatalogEntry &entry);

struct PostgresQueryCatalogRegistrar {
  explicit PostgresQueryCatalogRegistrar(const PostgresSharedQueryCatalogEntry &entry) {
    RegisterPostgresQuery(entry);
  }
};

const PostgresSharedQueryCatalogEntry &LookupPostgresQuery(const std::string &workload_name,
                                                            int query_id);

std::vector<int> PostgresQueriesForWorkload(const std::string &workload_name);

} // namespace accelerator_codegen::aux_state::engine::postgres
