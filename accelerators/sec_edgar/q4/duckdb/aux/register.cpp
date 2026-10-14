// GENERATED QUERY: SEC EDGAR Q4 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q4_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q4/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q4/cost_model.hpp"
#include "sec_edgar/q4/common/aux/module.hpp"
#include "sec_edgar/q4/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q4/layout.hpp"
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
namespace q4_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ4Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  return BuildSecNumRowidProbe(result, row, 1);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{4,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ4Layout::kAccelerator,
     "SELECT s.sic, t.tlabel, p.stmt, "
     "COUNT(DISTINCT s.cik) AS num_companies, "
     "SUM(n.value) AS total_value, "
     "AVG(n.value) AS avg_value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'EQ' "
     "AND s.sic BETWEEN 4000 AND 4999 "
     "AND n.value IS NOT NULL AND t.abstract = 0 "
     "GROUP BY s.sic, t.tlabel, p.stmt "
     "HAVING COUNT(DISTINCT s.cik) >= 2 "
     "ORDER BY total_value DESC "
     "LIMIT 500",
     "SELECT * FROM accel_sec_edgar_q4_aux()",
     "SELECT * FROM accel_sec_edgar_q4()",
     "SELECT * FROM accel_sec_edgar_q4_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
     "JOIN ("
     "    SELECT s2.sic, t2.tlabel, p2.stmt FROM num n2 "
     "    JOIN sub s2 ON n2.adsh = s2.adsh "
     "    JOIN tag t2 ON n2.tag = t2.tag AND n2.version = t2.version "
     "    JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND "
     "    n2.version = p2.version "
     "    WHERE n2.uom = 'USD' AND p2.stmt = 'EQ' AND s2.sic BETWEEN 4000 AND 4999 "
     "    AND n2.value IS NOT NULL AND t2.abstract = 0 "
     "    GROUP BY s2.sic, t2.tlabel, p2.stmt "
     "    HAVING COUNT(DISTINCT s2.cik) >= 2 "
     "    ORDER BY SUM(n2.value) DESC "
     "    LIMIT 500"
     ") top ON s.sic = top.sic AND t.tlabel = top.tlabel AND p.stmt = top.stmt "
     "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
     "AND n.value IS NOT NULL AND t.abstract = 0 "
     "ORDER BY n.adsh, n.tag, n.version LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ4Probe,
     core::kSecEdgarQ4CostModel}};

} // namespace q4_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// The scan pushes the bridge join down into DuckDB (duckdb_scan.cpp) instead of
// materializing num row by row.
using SecEdgarQ4Scan = query::SecEdgarQ4DuckDbScan;

// accel_sec_edgar_q4_aux() = the template defaults (4000, 4999, 'EQ', 'USD');
// accel_sec_edgar_q4_aux(sic_lo, sic_hi, stmt, uom) answers any parameters.
const std::vector<std::vector<::duckdb::LogicalType>> kOverloads{
    {::duckdb::LogicalType::INTEGER, ::duckdb::LogicalType::INTEGER,
     ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR}};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ4Layout::kAccelerator,
    "accel_sec_edgar_q4_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ4Config c{1, 2, 3, 4};
      return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4Module>(connection, CaptureTableList{}, c);
    },
    kOverloads}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ4Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ4Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ4Config c{1, 2, 3, 4};
      return MakeBinding<SecEdgarQ4Scan, SecEdgarQ4ParallelModule>(connection, CaptureTableList{}, c);
    },
    kOverloads}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
