// The hit contract (pipeline/prompts/engine_backend/docs/host_contract.md) on DuckDB: one accelerator
// of the workload on a database, its state not built. The first read through the SQL entry builds
// it; the second is a hit, with fallback SQL trapped (ACCEL_FALLBACK_TRAP=1, required).
//
// Usage: test_duckdb_hit_contract
// Env:   ACCEL_FALLBACK_TRAP=1 (required), ACCEL_TPCH_DB (copied into a scratch directory),
//        ACCEL_HIT_CONTRACT_QUERY (the TPC-H query used as the fixture, default 6)
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/register.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "catalog/query_catalog.hpp"
#include "duckdb.hpp"
#include "eval/tests/test_support/db_paths.hpp"

namespace {

namespace engine = accelerator_codegen::aux_state::engine;
namespace ts = accelerator_codegen::test_support;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

size_t ReadRows(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result && !result->HasError(), "read failed: " + (result ? result->GetError() : std::string("null result")));
  return result->RowCount();
}

} // namespace

int main() {
  try {
    Require(engine::FallbackTrapped(), "ACCEL_FALLBACK_TRAP=1 did not reach the harness through host.sh run");
    ts::ScratchDir scratch("hit_contract_duckdb");
    duckdb::DuckDB db(ts::PrivateDbCopy(ts::TpchDbPath(), scratch));
    duckdb::Connection connection(db);
    accelerator_codegen::RegisterAcceleratorFunctions(connection);

    const int query = ts::EnvOrInt("ACCEL_HIT_CONTRACT_QUERY", 6);
    const auto &entry = engine::LookupSharedQuery(engine::SharedWorkloadKind::Tpch, query);
    engine::duckdb::DuckdbRuntime accelerator(connection, entry, 10.0);
    Require(accelerator.Stats().builds == 0, "the state was built before the first read");

    // The first read plans before it builds, so at its snapshot the state does not exist yet: it may
    // be answered from SQL. Only the second read is held to the contract.
    engine::SetFallbackTrap(false);
    const size_t first_rows = ReadRows(connection, entry.duckdb_aux_sql);
    engine::SetFallbackTrap(std::nullopt);
    const auto after_first = accelerator.Stats();
    Require(after_first.builds == 1, "the first read did not build the state (builds=" +
                                         std::to_string(after_first.builds) + ")");

    const size_t second_rows = ReadRows(connection, entry.duckdb_aux_sql);
    const auto after_second = accelerator.Stats();
    Require(after_second.reads_hit == after_first.reads_hit + 1 &&
                after_second.reads_fallback == after_first.reads_fallback,
            "the second read was not a hit (hits " + std::to_string(after_second.reads_hit - after_first.reads_hit) +
                ", fallbacks " + std::to_string(after_second.reads_fallback - after_first.reads_fallback) + ")");
    Require(second_rows == first_rows, "the two reads returned different row counts");
    std::cout << "hit contract passed: Q" << query << " built on the first read, hit on the second" << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "hit contract violated: " << e.what() << std::endl;
    return 1;
  }
}
