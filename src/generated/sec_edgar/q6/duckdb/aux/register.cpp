// GENERATED QUERY: SEC EDGAR Q6 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q6_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q6/duckdb/aux/module.hpp"
#include "sec_edgar/q6/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q6/layout.hpp"
#include "sec_edgar/duckdb_probes.hpp"
#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
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
  return BuildSecNumRowidProbe(result, row, 1);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{6,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ6Layout::kAccelerator,
     "SELECT s.name, p.stmt, n.tag, p.plabel, "
     "SUM(n.value) AS total_value, COUNT(*) AS cnt "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
     "AND n.value IS NOT NULL "
     "GROUP BY s.name, p.stmt, n.tag, p.plabel "
     "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
     "LIMIT 200",
     "SELECT * FROM accel_sec_edgar_q6_aux()",
     "SELECT * FROM accel_sec_edgar_q6()",
     "SELECT * FROM accel_sec_edgar_q6_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "JOIN ("
     "    SELECT s2.name, p2.stmt, n2.tag, p2.plabel FROM num n2 "
     "    JOIN sub s2 ON n2.adsh = s2.adsh "
     "    JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND "
     "    n2.version = p2.version "
     "    WHERE n2.uom = 'USD' AND p2.stmt = 'IS' AND s2.fy = 2023 "
     "    AND n2.value IS NOT NULL "
     "    GROUP BY s2.name, p2.stmt, n2.tag, p2.plabel "
     "    ORDER BY SUM(n2.value) DESC, s2.name, n2.tag, p2.plabel "
     "    LIMIT 200"
     ") top ON s.name = top.name AND p.stmt = top.stmt AND n.tag = top.tag "
     "AND p.plabel IS NOT DISTINCT FROM top.plabel "
     "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
     "AND n.value IS NOT NULL "
     "ORDER BY n.adsh, n.tag, n.version LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ6Probe}};

} // namespace q6_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct SecEdgarQ6Scan : SecEdgarQ6InitialScan {
  explicit SecEdgarQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ6NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ6SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ6PreInput &)> &consumer) override {
    ScanQuery(connection,
              "SELECT adsh, tag, version, stmt, plabel FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          result.GetValue(4, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ6Layout::kAccelerator,
    "accel_sec_edgar_q6_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ6Config c{1, 2, 3};
      return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6Module>(connection, CaptureTableList{}, c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ6Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ6Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ6Config c{1, 2, 3};
      return MakeBinding<SecEdgarQ6Scan, SecEdgarQ6ParallelModule>(connection, CaptureTableList{}, c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
