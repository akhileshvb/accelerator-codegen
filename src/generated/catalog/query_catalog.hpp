#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::engine {

enum class SharedWorkloadKind { Tpch, SecEdgar };

struct SharedQueryArguments {
  std::vector<core::AuxValue> values;
};

struct SharedProbe {
  core::TableId table = 0;
  core::MutationKind kind = core::MutationKind::Update;
  std::string update_sql;
  std::string restore_sql;
  core::AuxRow old_row;
  core::AuxRow new_row;
  std::vector<core::ColumnId> updated_columns;
  double current_value = 0.0;
  double write_delta = 0.0;
};

struct SharedQueryCatalogEntry {
  int query_id = 0;
  SharedWorkloadKind workload = SharedWorkloadKind::Tpch;
  const char *workload_name = nullptr;
  const char *accelerator_id = nullptr;
  const char *base_sql = nullptr;
  const char *duckdb_aux_sql = nullptr;
  const char *postgres_base_kernel_sql = nullptr;
  const char *postgres_aux_sql = nullptr;
  const char *probe_select_sql = nullptr;
  SharedQueryArguments benchmark_arguments;
  core::TableId mutation_table = 0;
  bool compare_unordered = false;
  using ProbeBuilder = std::function<SharedProbe(
      ::duckdb::MaterializedQueryResult &, ::duckdb::idx_t row)>;
  ProbeBuilder build_probe;
};

const SharedQueryCatalogEntry &LookupSharedQuery(SharedWorkloadKind workload,
                                               int query_id);
const SharedQueryCatalogEntry &LookupSharedQuery(const std::string &workload_name,
                                               int query_id);

std::vector<int> SharedQueriesForWorkload(SharedWorkloadKind workload);
std::vector<int> SharedQueriesForWorkload(const std::string &workload_name);

SharedProbe FindSharedProbe(::duckdb::Connection &connection,
                            const SharedQueryCatalogEntry &entry);

core::MutationBatch BuildSharedMutation(const SharedQueryCatalogEntry &entry,
                                        const SharedProbe &probe);

core::QueryArguments ToCoreArguments(const SharedQueryArguments &arguments);

} // namespace accelerator_codegen::aux_state::engine
