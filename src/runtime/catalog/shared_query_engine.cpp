// SCAFFOLDING
#include "accelerator_codegen/runtime/catalog/shared_query_engine.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine {
namespace {

const char *RequireSql(const char *sql, const char *label) {
  if (!sql || !*sql) {
    throw std::runtime_error(std::string("missing shared query ") + label);
  }
  return sql;
}

} // namespace

const char *SharedQueryEngine::KernelSql(const SharedQueryCatalogEntry &entry,
                                         SharedQueryPath path) {
  switch (path) {
  case SharedQueryPath::BaseKernel:
    return RequireSql(entry.postgres_base_kernel_sql, "base kernel SQL");
  case SharedQueryPath::AuxStateful:
    return RequireSql(entry.postgres_aux_sql, "aux kernel SQL");
  }
  throw std::runtime_error("unknown shared query path");
}

bool SharedQueryEngine::HasSharedQuery(SharedWorkloadKind workload,
                                       int query_id) {
  try {
    (void)LookupSharedQuery(workload, query_id);
    return true;
  } catch (...) {
    return false;
  }
}

bool SharedQueryEngine::HasSharedQuery(const std::string &workload_name,
                                       int query_id) {
  try {
    (void)LookupSharedQuery(workload_name, query_id);
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace accelerator_codegen::aux_state::engine
