// GENERATED BACKEND — MySQL query catalog.
// MySQL query catalog.  The backend names no query: each query registers its own
// entry from src/generated/{bench}/q{N}/mysql/aux/catalog.cpp through a static
// MysqlQueryCatalogRegistrar.  Lookups iterate the registry, sorted by query id.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

struct MysqlSharedQueryArguments {
  std::vector<core::AuxValue> values;
};

struct MysqlSharedQueryCatalogEntry {
  int query_id = 0;
  const char *workload_name = nullptr;
  const char *accelerator_id = nullptr;
  const char *base_sql = nullptr;
  const char *accelerator_sql = nullptr;
  const char *aux_sql = nullptr;
};

// Registers (replacing an entry with the same workload and id).  Entries are
// held stably; the strings must outlive the process (string literals).
void RegisterMysqlQuery(const MysqlSharedQueryCatalogEntry &entry);

struct MysqlQueryCatalogRegistrar {
  explicit MysqlQueryCatalogRegistrar(const MysqlSharedQueryCatalogEntry &entry) {
    RegisterMysqlQuery(entry);
  }
};

const MysqlSharedQueryCatalogEntry &LookupMysqlQuery(const std::string &workload_name,
                                                            int query_id);

std::vector<int> MysqlQueriesForWorkload(const std::string &workload_name);

} // namespace accelerator_codegen::aux_state::engine::mysql
