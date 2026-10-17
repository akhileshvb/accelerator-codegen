// SCAFFOLDING
#pragma once

#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"

namespace accelerator_codegen::aux_state::harness {

namespace aux_engine = ::accelerator_codegen::aux_state::engine;

void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const aux_engine::SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated);

// Same as CompareAuxRowsToBase, but against an arbitrary DuckDB SQL string
// (used for TPC-H substitution-parameter sweeps).
void CompareAuxRowsToSql(duckdb::Connection &connection,
                         const aux_engine::SharedQueryCatalogEntry &entry,
                         const std::string &base_sql,
                         const std::vector<core::AuxRow> &accelerated);

void RequireSharedMatchesBase(duckdb::Connection &connection,
                              aux_engine::duckdb::DuckdbRuntime &runtime,
                              const aux_engine::SharedQueryCatalogEntry &entry);

void RunDifferentialCheck(const std::string &db_path,
                          const aux_engine::SharedQueryCatalogEntry &entry);

void RunIncrementalDifferentialCheck(const std::string &db_path,
                                     const aux_engine::SharedQueryCatalogEntry &entry);

struct NamedWrite {
  std::string name;
  std::string sql;
};

// The writes RunSqlWritesDifferentialCheck runs for `entry`, in order.
std::vector<NamedWrite> TpchSqlWrites(duckdb::Connection &connection,
                                      const aux_engine::SharedQueryCatalogEntry &entry, int rounds);

// Apply alternating mini-chunk RF2/RF1 as captured SQL (ApplyTpchRfChunk), then
// require Execute matches DuckDB base_sql. Escalates chunk size until the DuckDB
// answer changes so the write is a real invalidation for this query. db_path
// must be a writable work copy (mutations are permanent).
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

void RunRfChunkDifferentialCheck(const std::string &db_path,
                                 const aux_engine::SharedQueryCatalogEntry &entry,
                                 int num_chunks = 4, int order_count = 1);

// Rigorous post-analysis write audit: RO, then several distinct invalidating
// point probes applied sequentially (restore after each), then optional RF bulk
// chunks. db_path must be a writable work copy (RF mutations are permanent).
// Env (optional): ACCEL_ADV_MAX_PROBES, ACCEL_ADV_MIN_PROBES,
// ACCEL_ADV_MIN_CHUNKS, ACCEL_ADV_REQUIRE_BULK.
void RunAdversarialWriteDifferentialCheck(
    const std::string &db_path, const aux_engine::SharedQueryCatalogEntry &entry,
    int max_probes = 5, int min_chunks = 1, bool require_bulk = false);

} // namespace accelerator_codegen::aux_state::harness
