// GENERATED QUERY: SEC EDGAR Q6 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q6_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q6/duckdb/aux/duckdb_scan.hpp"
#include "sec_edgar/q6/cost_model.hpp"
#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "sec_edgar/q6/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q6/layout.hpp"
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
namespace q6_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ6Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  // The shared probe carries the physical num row; the module is delivered
  // the columns it projects (SecEdgarQ6Columns::kNumAttnums).
  auto probe = BuildSecNumRowidProbe(result, row, 1);
  const auto project = [](const core::AuxRow &full) {
    core::AuxRow out;
    for (const auto attnum : query::SecEdgarQ6Columns::kNumAttnums) {
      out.values.push_back(full.values.at(static_cast<std::size_t>(attnum)));
    }
    return out;
  };
  probe.old_row = project(probe.old_row);
  probe.new_row = project(probe.new_row);
  probe.updated_columns = {query::SecEdgarQ6Columns::kNumValue};
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{6,
     "sec_edgar",
     core::SecEdgarQ6Layout::kAccelerator,
     "SELECT s.name, p.stmt, n.tag, p.plabel, "
     "SUM(n.value) AS total_value, COUNT(*) AS cnt "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = $3 AND p.stmt = $2 AND s.fy = $1 "
     "AND n.value IS NOT NULL "
     "GROUP BY s.name, p.stmt, n.tag, p.plabel "
     "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
     "LIMIT 200",
     "SELECT * FROM accel_sec_edgar_q6_aux($1, $2, $3)",
     "SELECT * FROM accel_sec_edgar_q6()",
     "SELECT * FROM accel_sec_edgar_q6_aux($1, $2, $3)",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "JOIN ("
     "    SELECT s2.name, p2.stmt, n2.tag, p2.plabel FROM num n2 "
     "    JOIN sub s2 ON n2.adsh = s2.adsh "
     "    JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND "
     "    n2.version = p2.version "
     "    WHERE n2.uom = $3 AND p2.stmt = $2 AND s2.fy = $1 "
     "    AND n2.value IS NOT NULL "
     "    GROUP BY s2.name, p2.stmt, n2.tag, p2.plabel "
     "    ORDER BY SUM(n2.value) DESC, s2.name, n2.tag, p2.plabel "
     "    LIMIT 200"
     ") top ON s.name = top.name AND p.stmt = top.stmt AND n.tag = top.tag "
     "AND p.plabel IS NOT DISTINCT FROM top.plabel "
     "WHERE n.uom = $3 AND p.stmt = $2 AND s.fy = $1 "
     "AND n.value IS NOT NULL "
     "ORDER BY n.adsh, n.tag, n.version LIMIT 1",
     {{core::AuxValue::Null(core::AuxType::Int64), core::AuxValue::Null(core::AuxType::String),
       core::AuxValue::Null(core::AuxType::String)}},
     1,
     false,
     BuildSecEdgarQ6Probe,
     core::kSecEdgarQ6CostModel,
     "SELECT s.name, p.stmt, n.tag, p.plabel, SUM(n.value) AS total_value, COUNT(*) "
     "AS cnt FROM num n JOIN sub s ON n.adsh = s.adsh JOIN pre p ON n.adsh = p.adsh "
     "AND n.tag = p.tag AND n.version = p.version WHERE n.uom = $3 AND p.stmt = $2 "
     "AND s.fy = TRY_CAST($1 AS INTEGER) AND n.value IS NOT NULL GROUP BY s.name, "
     "p.stmt, n.tag, p.plabel ORDER BY total_value DESC, s.name, n.tag, p.plabel "
     "LIMIT 200"}};

} // namespace q6_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

using SecEdgarQ6Scan = query::SecEdgarQ6DuckdbScan;

// The tables engine capture watches for the module (columns per
// SecEdgarQ6Columns).
CaptureTableList CaptureTables(const SecEdgarQ6Config &c) {
  return {{"num", c.num_table}, {"sub", c.sub_table}, {"pre", c.pre_table}};
}

// accel_sec_edgar_q6_aux(fy BIGINT, stmt VARCHAR, uom VARCHAR) answers any
// values from the same state.
const std::vector<::duckdb::LogicalType> kArgumentTypes{
    ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ6Layout::kAccelerator,
    "accel_sec_edgar_q6_aux",
    kArgumentTypes,
    [](::duckdb::Connection &connection) {
      const SecEdgarQ6Config c{1, 2, 3};
      return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6Module>(connection, CaptureTables(c), c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ6Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ6Layout::kAccelerator + "_aux_parallel",
    kArgumentTypes,
    [](::duckdb::Connection &connection) {
      const SecEdgarQ6Config c{1, 2, 3};
      return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6ParallelModule>(connection, CaptureTables(c), c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
