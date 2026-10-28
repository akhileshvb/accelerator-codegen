// SCAFFOLDING
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/harness/suite.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

// The checks below compare with the entry's base SQL bound to `arguments`; without the argument, to the
// default binding (an entry without one needs the argument).
void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const aux_engine::SharedQueryCatalogEntry &entry,
                          const core::QueryArguments &arguments,
                          const std::vector<core::AuxRow> &accelerated);
void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const aux_engine::SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated);

// Same as CompareAuxRowsToBase, but against an arbitrary DuckDB SQL string
// (used for TPC-H substitution-parameter sweeps).
void CompareAuxRowsToSql(duckdb::Connection &connection,
                         const aux_engine::SharedQueryCatalogEntry &entry,
                         const std::string &base_sql,
                         const std::vector<core::AuxRow> &accelerated);

// The accelerator, read with `arguments`, answers as the base SQL bound to them.
void RequireSharedMatchesBase(duckdb::Connection &connection,
                              aux_engine::duckdb::DuckdbRuntime &runtime,
                              const aux_engine::SharedQueryCatalogEntry &entry,
                              const core::QueryArguments &arguments);
void RequireSharedMatchesBase(duckdb::Connection &connection,
                              aux_engine::duckdb::DuckdbRuntime &runtime,
                              const aux_engine::SharedQueryCatalogEntry &entry);

// One database, connection and runtime for every phase of a differential run: build the module once (runtime.Build()),
// then run the checks below on it. Each leaves the data as it found it (writes are undone through the runtime, which
// maintains the state for the undo too), except CheckRefreshWrites and CheckRfChunks (they go last); CheckNoDrift ends the run with the
// only from-scratch rebuild.
// The session reads and compares with `arguments` (an entry with a default binding: that).
struct DifferentialSession {
  DifferentialSession(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry);
  DifferentialSession(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
                      core::QueryArguments arguments);

  const aux_engine::SharedQueryCatalogEntry &entry;
  core::QueryArguments arguments;
  duckdb::DuckDB database;
  duckdb::Connection connection;
  aux_engine::duckdb::DuckdbRuntime runtime;
};

// The accelerator answers the query as DuckDB does on the data as it stands.
void CheckReadOnly(DifferentialSession &session);

// One result-changing write and its undo.
void CheckIncrementalWrite(DifferentialSession &session);

// Limits of the adversarial writes (ACCEL_ADV_MAX_PROBES, _MIN_PROBES, _MIN_CHUNKS, _REQUIRE_BULK override the defaults).
struct AdversarialPlan {
  int max_probes = 5;
  int min_probes = 2;
  int min_chunks = 1;
  bool require_bulk = false;
};
AdversarialPlan AdversarialPlanFromEnv(int max_probes, int min_chunks, bool require_bulk);

// Several distinct result-changing point probes, each applied and undone, then a sequence of them undone in reverse.
void CheckAdversarialWrites(DifferentialSession &session, const AdversarialPlan &plan);

// The TPC-H refresh chunks (permanent) when the plan requires bulk writes.
void CheckRefreshWrites(DifferentialSession &session, const AdversarialPlan &plan);
void CheckRfChunks(DifferentialSession &session, int num_chunks = 4, int order_count = 1);

// Rebuilds the state from scratch on the data as it stands: `answers` (default: the query's own answer) must be the
// same as the maintained state gave before, and the query's.
void CheckNoDrift(DifferentialSession &session);
void CheckNoDrift(DifferentialSession &session, const std::function<suite::Rows()> &answers);

// The same checks on a database of their own (a fresh session each).
void RunDifferentialCheck(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
                          const core::QueryArguments &arguments);
void RunDifferentialCheck(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry);

void RunIncrementalDifferentialCheck(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
                                     const core::QueryArguments &arguments);
void RunIncrementalDifferentialCheck(const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry);

struct NamedWrite {
  std::string name;
  std::string sql;
};

// The writes RunSqlWritesDifferentialCheck runs for `entry`, in order.
std::vector<NamedWrite> TpchSqlWrites(duckdb::Connection &connection,
                                      const aux_engine::SharedQueryCatalogEntry &entry, int rounds);

// Writes of every TPC-H kind (RF1/RF2, lineitem / customer updates, parts and
// suppliers deleted and inserted with their partsupp rows, a supplier-only
// update), run as SQL and captured by the engine; after each one the
// accelerator must match the query.
void RunSqlWritesDifferentialCheck(const std::string &db_path,
                                   const aux_engine::SharedQueryCatalogEntry &entry, int rounds);

// Writes big enough, or of a kind the module cannot apply incrementally, that
// the policy (not a forced action) chooses a rebuild: each write is applied
// and then undone; after each, the accelerator must match the query both while
// the rebuild may still be running (SQL fallback) and once it has settled
// (served from the state, no new fallback). Env: ACCEL_LOG_MAINTENANCE_ACTIONS=1
// prints the action the policy chose for each write.
void RunBulkWritesDifferentialCheck(const std::string &db_path,
                                    const aux_engine::SharedQueryCatalogEntry &entry);

// Alternating mini-chunk RF2/RF1 as captured SQL (ApplyTpchRfChunk), the accelerator matching the query after each.
// Escalates the chunk size until the DuckDB answer changes, so the write is a real invalidation for this query.
// The data stays changed.
void RunRfChunkDifferentialCheck(const std::string &db_path,
                                 const aux_engine::SharedQueryCatalogEntry &entry,
                                 int num_chunks = 4, int order_count = 1);

// Rigorous post-analysis write audit: RO, then the adversarial probes, optional RF bulk chunks and the drift check.
// db_path must be a writable work copy (RF mutations are permanent). Env: see AdversarialPlan.
void RunAdversarialWriteDifferentialCheck(
    const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
    const core::QueryArguments &arguments, int max_probes = 5, int min_chunks = 1, bool require_bulk = false);
void RunAdversarialWriteDifferentialCheck(
    const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
    int max_probes = 5, int min_chunks = 1, bool require_bulk = false);

} // namespace accelerator_codegen::aux_state::harness
