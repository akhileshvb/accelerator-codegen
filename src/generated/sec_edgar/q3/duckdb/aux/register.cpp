// GENERATED QUERY: SEC EDGAR Q3 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q3_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "sec_edgar/q3/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q3/layout.hpp"
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
namespace q3_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ3Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  return BuildSecNumRowidProbe(result, row, 1);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{3,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ3Layout::kAccelerator,
     "SELECT s.name, s.cik, SUM(n.value) AS total_value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
     "GROUP BY s.name, s.cik "
     "HAVING SUM(n.value) > ("
     "    SELECT AVG(sub_total) FROM ("
     "        SELECT SUM(n2.value) AS sub_total "
     "        FROM num n2 "
     "        JOIN sub s2 ON n2.adsh = s2.adsh "
     "        WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
     "        GROUP BY s2.cik"
     "    ) avg_sub"
     ") "
     "ORDER BY total_value DESC "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q3_aux()",
     "SELECT * FROM accel_sec_edgar_q3()",
     "SELECT * FROM accel_sec_edgar_q3_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN ("
     "    SELECT s2.cik FROM num n2 "
     "    JOIN sub s2 ON n2.adsh = s2.adsh "
     "    WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
     "    GROUP BY s2.name, s2.cik "
     "    HAVING SUM(n2.value) > ("
     "        SELECT AVG(sub_total) FROM ("
     "            SELECT SUM(n3.value) AS sub_total "
     "            FROM num n3 JOIN sub s3 ON n3.adsh = s3.adsh "
     "            WHERE n3.uom = 'USD' AND s3.fy = 2022 AND n3.value IS NOT NULL "
     "            GROUP BY s3.cik"
     "        ) avg_sub"
     "    ) "
     "    ORDER BY SUM(n2.value) DESC "
     "    LIMIT 100"
     ") top ON s.cik = top.cik "
     "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.adsh LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ3Probe}};

} // namespace q3_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct SecEdgarQ3Scan : SecEdgarQ3InitialScan {
  explicit SecEdgarQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ3NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(2, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, cik, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row))),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ3Layout::kAccelerator,
    "accel_sec_edgar_q3_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ3Config c{1, 2};
      return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3Module>(connection, CaptureTableList{}, c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ3Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ3Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ3Config c{1, 2};
      return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3ParallelModule>(connection, CaptureTableList{}, c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
