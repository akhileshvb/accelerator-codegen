// GENERATED BACKEND
#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <functional>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_cost_model_path.hpp"
#include "substrate/aux/core/aux_types.hpp"
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
  // Where the query keeps its maintenance cost models (<bench>/q<N>/cost_model.hpp).
  core::AuxCostModelLocation cost_model;
};

// ---- the registry: each query's directory registers its entries ----
//
// accelerators/<bench>/q<N>/duckdb/aux/register.cpp holds
//   const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{...}};
// next to its DuckDB AuxQueryRegistrar (runtime/duckdb/aux_query_registry.hpp).
void RegisterSharedQuery(SharedQueryCatalogEntry entry);
struct SharedQueryRegistrar {
  explicit SharedQueryRegistrar(SharedQueryCatalogEntry entry) { RegisterSharedQuery(std::move(entry)); }
};
// Another accelerator serving a registered query (same SQL, probes and
// arguments) under its own id and aux SQL, e.g. a Verus kernel for it.
void RegisterSharedQueryVariant(std::string accelerator_id, SharedWorkloadKind workload, int query_id,
                                std::string duckdb_aux_sql);
struct SharedQueryVariantRegistrar {
  SharedQueryVariantRegistrar(std::string accelerator_id, SharedWorkloadKind workload, int query_id,
                              std::string duckdb_aux_sql) {
    RegisterSharedQueryVariant(std::move(accelerator_id), workload, query_id, std::move(duckdb_aux_sql));
  }
};
// The entry an accelerator id serves: a registered entry, a variant,
// `<id>_aux_parallel` of a registered entry, or a Verus kit accelerator.
// Null if none.
const SharedQueryCatalogEntry *FindSharedQueryByAccelerator(const std::string &accelerator_id);

const SharedQueryCatalogEntry &LookupSharedQuery(SharedWorkloadKind workload,
                                               int query_id);
// A Verus accelerator registered through the host kit (verus_kit.hpp): its
// catalog query's entry under the accelerator's id (table function
// accel_<id>).  Null if no glue with that id is registered.
const SharedQueryCatalogEntry *VerusKitCatalogEntry(const std::string &accelerator_id);

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

// ---- batched writes: many distinct rows per write transaction ----
//
// An additive rowid probe (update_sql "UPDATE <table> SET <column> = <column>
// + <delta> WHERE rowid = <id>"), the form every SEC-EDGAR probe has.
struct RowidProbeTarget {
  std::string table;
  std::string column;
  int64_t rowid = 0;
};
// Throws if `probe` is not an additive rowid probe.
RowidProbeTarget ParseRowidProbe(const SharedProbe &probe);

// A pool of distinct rows of one table/column for batched writes: the
// catalog's invalidating candidates (probe_select_sql, the rows feeding the
// query result) first, then filler rows of the same table/column if the
// query has fewer candidates than wanted.
struct BatchWritePool {
  std::string table;
  std::string column;
  std::vector<SharedProbe> probes;  // distinct rowids, invalidating first
  std::vector<int64_t> rowids;      // probes[i]'s rowid
  std::size_t invalidating = 0;     // probes[0, invalidating) are candidates
};

// Up to `want` distinct rows (throws if fewer than `min_rows`).
// `filler_select_sql` has probe_select_sql's columns (build_probe reads it)
// and a trailing LIMIT, which is replaced; null: no filler.
BatchWritePool CollectBatchWritePool(::duckdb::Connection &connection,
                                     const SharedQueryCatalogEntry &entry,
                                     std::size_t want, std::size_t min_rows,
                                     const char *filler_select_sql);

// One mutation carrying every probe's current old/new row (one table).
core::MutationBatch BuildSharedBatchMutation(
    const SharedQueryCatalogEntry &entry,
    const std::vector<const SharedProbe *> &probes);

core::QueryArguments ToCoreArguments(const SharedQueryArguments &arguments);

// ---- value helpers for the queries' probe builders ----
namespace catalog_values {

inline void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

inline int64_t DecimalRaw(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  try {
    return static_cast<int64_t>(
        std::llround(std::stod(value.ToString()) * 100.0));
  } catch (const std::exception &) {
    return 0;
  }
}

inline core::AuxValue Text(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::String);
  }
  return core::AuxValue::Of(core::AuxType::String, value.ToString());
}

inline int64_t SafeInt64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  try {
    return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
  } catch (const std::exception &) {
    return 0;
  }
}

inline core::AuxValue Int(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::Int64);
  }
  return core::AuxValue::Of(core::AuxType::Int64, SafeInt64(value));
}

inline std::string SqlLiteral(const std::string &value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

inline core::AuxValue Number(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::Double);
  }
  return core::AuxValue::Of(core::AuxType::Double,
                            std::stod(value.ToString()));
}

inline core::AuxValue Date(int32_t days) {
  return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days});
}

inline core::AuxValue Decimal(int64_t raw, int32_t scale) {
  return core::AuxValue::Of(core::AuxType::ScaledDecimal,
                            core::ScaledDecimal{raw, scale});
}

inline int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  const auto date = value.GetValue<::duckdb::date_t>();
  return ::duckdb::Date::EpochDays(date);
}

} // namespace catalog_values

} // namespace accelerator_codegen::aux_state::engine
