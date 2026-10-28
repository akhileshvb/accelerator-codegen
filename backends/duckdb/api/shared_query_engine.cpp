// SCAFFOLDING
#include "backends/duckdb/api/shared_query_engine.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine {
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
