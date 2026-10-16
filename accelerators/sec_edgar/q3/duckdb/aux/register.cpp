// GENERATED QUERY: SEC EDGAR Q3 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q3_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q3/duckdb/aux/module.hpp"
#include "sec_edgar/q3/cost_model.hpp"
#include "sec_edgar/q3/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q3/layout.hpp"
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
     BuildSecEdgarQ3Probe,
     core::kSecEdgarQ3CostModel}};

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
  // Pushes the value filter and the per-(adsh, uom) aggregation into DuckDB
  // (one parallel GROUP BY over num) instead of streaming every num row
  // through C++.
  void ScanNumAggregates(
      const std::function<void(const SecEdgarQ3NumAggInput &)> &consumer) override {
    ScanQueryDecoded(
        connection,
        "SELECT adsh, uom, SUM(value), COUNT(*) FROM num "
        "WHERE value IS NOT NULL AND adsh IS NOT NULL AND uom IS NOT NULL "
        "GROUP BY adsh, uom",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return SecEdgarQ3NumAggInput{chunk.GetValue(0, row).ToString(),
                                       chunk.GetValue(1, row).ToString(),
                                       chunk.GetValue(2, row).GetValue<double>(),
                                       chunk.GetValue(3, row).GetValue<int64_t>()};
        },
        [&](SecEdgarQ3NumAggInput &&input) { consumer(input); });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ3SubInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT adsh, name, cik, fy FROM sub",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          SecEdgarQ3SubInput input;
          input.adsh = chunk.GetValue(0, row).ToString();
          const auto name = chunk.GetValue(1, row);
          input.name_is_null = name.IsNull();
          if (!input.name_is_null) {
            input.name = name.ToString();
          }
          const auto cik = chunk.GetValue(2, row);
          input.cik_is_null = cik.IsNull();
          input.cik = input.cik_is_null ? 0 : SafeInt32(cik);
          const auto fy = chunk.GetValue(3, row);
          input.fy_is_null = fy.IsNull();
          input.fy = input.fy_is_null ? 0 : SafeInt32(fy);
          return input;
        },
        [&](SecEdgarQ3SubInput &&input) { consumer(input); });
  }
  ::duckdb::Connection &connection;
};

// accel_sec_edgar_q3_aux() uses the defaults (fy 2022, uom 'USD');
// accel_sec_edgar_q3_aux(fy BIGINT, uom VARCHAR) answers any (fy, uom).
const std::vector<std::vector<::duckdb::LogicalType>> kOverloads{
    {::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR}};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ3Layout::kAccelerator,
    "accel_sec_edgar_q3_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ3Config c{1, 2};
      return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3Module>(connection, CaptureTableList{}, c);
    },
    kOverloads}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ3Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ3Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ3Config c{1, 2};
      return MakeBinding<SecEdgarQ3Scan, SecEdgarQ3ParallelModule>(connection, CaptureTableList{}, c);
    },
    kOverloads}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
