// GENERATED BACKEND
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
  // An idempotent write (SET col = constant): repeating it changes nothing,
  // so the probe alternates update / restore (old/new rows swapped) to keep
  // each reported mutation equal to what the write did.
  bool toggles = false;
  bool flipped = false; // the next write is the restore
  // An idempotent write whose repeated mutation leaves the module's state as
  // it is (e.g. a new minimum set again): the same write every time.
  // Otherwise (neither flag) the write is additive (x = x + c) and each
  // write's reported rows advance by the same step.
  bool repeats = false;
};

// The SQL of the probe's next write, and the step after it is applied.
const std::string &ProbeWriteSql(const SharedProbe &probe);
void AdvanceProbe(SharedProbe &probe);

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
// TPC-H Q21 served by the Verus-verified kernel (accelerator "tpch_q21_verus",
// table function accel_tpch_q21_verus).  Same SQL / probes as Q21.
const SharedQueryCatalogEntry &TpchQ21VerusCatalogEntry();

const SharedQueryCatalogEntry &LookupSharedQuery(const std::string &workload_name,
                                               int query_id);

bool IsParallelAuxAccelerator(const char *accelerator_id);
const SharedQueryCatalogEntry &LookupSharedQueryParallel(SharedWorkloadKind workload,
                                                       int query_id);
const SharedQueryCatalogEntry &LookupSharedQueryParallel(const std::string &workload_name,
                                                       int query_id);

std::vector<int> SharedQueriesForWorkload(SharedWorkloadKind workload);
std::vector<int> SharedQueriesForWorkload(const std::string &workload_name);

SharedProbe FindSharedProbe(::duckdb::Connection &connection,
                            const SharedQueryCatalogEntry &entry);

// Expand/replace the trailing LIMIT on probe_select_sql (bench + differential).
std::string ExpandProbeSelectLimit(const char *probe_select_sql, int limit);

// Fingerprint of entry.base_sql (stable enough to detect result-changing writes).
std::string FingerprintSharedBaseSql(::duckdb::Connection &connection,
                                     const SharedQueryCatalogEntry &entry);

// True iff probe.update_sql changes FingerprintSharedBaseSql (txn rolled back).
bool SharedProbeChangesBaseResult(::duckdb::Connection &connection,
                                  const SharedQueryCatalogEntry &entry,
                                  const SharedProbe &probe,
                                  const std::string &before_fp);

// Catalog probe_select candidates whose update_sql invalidates this query's
// DuckDB answer. Used by write-frequency benches so maintenance is exercised.
// Throws if zero invalidating probes are found.
std::vector<SharedProbe> CollectInvalidatingSharedProbes(
    ::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
    int candidate_limit = 256, int max_keep = 64);

// First invalidating probe (same filter as CollectInvalidatingSharedProbes).
SharedProbe FindInvalidatingSharedProbe(::duckdb::Connection &connection,
                                        const SharedQueryCatalogEntry &entry);

core::MutationBatch BuildSharedMutation(const SharedQueryCatalogEntry &entry,
                                        const SharedProbe &probe);

core::QueryArguments ToCoreArguments(const SharedQueryArguments &arguments);

} // namespace accelerator_codegen::aux_state::engine
