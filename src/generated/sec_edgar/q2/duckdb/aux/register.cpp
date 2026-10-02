// GENERATED QUERY: SEC EDGAR Q2 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q2_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q2/duckdb/aux/module.hpp"
#include "sec_edgar/q2/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q2/layout.hpp"
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
     BuildSecEdgarQ2Probe}};

} // namespace q2_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct SecEdgarQ2Scan : SecEdgarQ2InitialScan {
  explicit SecEdgarQ2Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ2NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, uom, value FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(3, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull()});
              });
  }
  void ScanSub(
      const std::function<void(const SecEdgarQ2SubInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, name, fy FROM sub",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(2, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ2Layout::kAccelerator,
    "accel_sec_edgar_q2_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ2Config c{1, 2};
      return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2Module>(connection, CaptureTableList{}, c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ2Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ2Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ2Config c{1, 2};
      return MakeBinding<SecEdgarQ2Scan, SecEdgarQ2ParallelModule>(connection, CaptureTableList{}, c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
