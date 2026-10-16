// GENERATED QUERY: SEC EDGAR Q2 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q2_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q2/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q2/cost_model.hpp"
#include "sec_edgar/q2/duckdb/aux/module.hpp"
#include "sec_edgar/q2/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q2/layout.hpp"
#include "sec_edgar/common/duckdb_probes.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q2_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ2Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  return BuildSecNumRowidProbe(result, row, 1);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{2,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ2Layout::kAccelerator,
     "SELECT s.name, n.tag, n.value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN ("
     "    SELECT adsh, tag, MAX(value) AS max_value "
     "    FROM num "
     "    WHERE uom = 'pure' AND value IS NOT NULL "
     "    GROUP BY adsh, tag"
     ") m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
     "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.value DESC, s.name, n.tag "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q2_aux()",
     "SELECT * FROM accel_sec_edgar_q2()",
     "SELECT * FROM accel_sec_edgar_q2_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN ("
     "    SELECT adsh, tag, MAX(value) AS max_value "
     "    FROM num "
     "    WHERE uom = 'pure' AND value IS NOT NULL "
     "    GROUP BY adsh, tag"
     ") m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
     "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.value DESC, s.name, n.tag LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ2Probe,
     core::kSecEdgarQ2CostModel}};

} // namespace q2_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// The data access is columnar SQL on the runtime's connection (duckdb_scan.cpp).
using SecEdgarQ2Scan = SecEdgarQ2DuckdbScan;

// accel_sec_edgar_q2_aux() uses the template defaults (fy 2022, uom 'pure');
// accel_sec_edgar_q2_aux(fy BIGINT, uom VARCHAR) binds the template literals.
const std::vector<std::vector<::duckdb::LogicalType>> kOverloads{
    {::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR}};

// The module reads positions in the full num / sub rows.
SecEdgarQ2Config Config() { return SecEdgarQ2Config{1, 2}; }

CaptureTableList CaptureTables(const SecEdgarQ2Config &c) {
  return {{"num", c.num_table}, {"sub", c.sub_table}};
}

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ2Layout::kAccelerator,
    "accel_sec_edgar_q2_aux",
    {},
    [](::duckdb::Connection &connection) {
      const auto c = Config();
      return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2Module>(connection, CaptureTables(c), c);
    },
    kOverloads}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ2Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ2Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      auto c = Config();
      c.build_threads = 0; // parallel variant: automatic worker count
      return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2ParallelModule>(connection, CaptureTables(c), c);
    },
    kOverloads}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
