// GENERATED QUERY: SEC EDGAR Q24 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q24_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q24/duckdb/aux/module.hpp"
#include "sec_edgar/q24/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q24/layout.hpp"
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
namespace q24_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;
using sec_edgar_probes::BuildSecNumRowidProbe;

SharedProbe BuildSecEdgarQ24Probe(::duckdb::MaterializedQueryResult &result,
                                  ::duckdb::idx_t row) {
  return BuildSecNumRowidProbe(result, row, 101);
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{24,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ24Layout::kAccelerator,
     "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
     "FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh "
     "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
     "AND n.value IS NOT NULL AND p.adsh IS NULL "
     "GROUP BY n.tag, n.version "
     "HAVING COUNT(*) > 10 "
     "ORDER BY cnt DESC "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT * FROM accel_sec_edgar_q24()",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.coreg, "
     "n.value FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 "
     "AND 20231231 AND n.value IS NOT NULL AND p.adsh IS NULL "
     "AND (n.tag, n.version) IN ("
     "  SELECT n2.tag, n2.version FROM num n2 "
     "  LEFT JOIN pre p2 ON n2.tag = p2.tag AND n2.version = p2.version AND "
     "  n2.adsh = p2.adsh WHERE n2.uom = 'USD' AND n2.ddate BETWEEN 20230101 "
     "  AND 20231231 AND n2.value IS NOT NULL AND p2.adsh IS NULL "
     "  GROUP BY n2.tag, n2.version HAVING COUNT(*) > 10 "
     "  ORDER BY COUNT(*) DESC "
     "  LIMIT 100"
     ") ORDER BY n.value DESC LIMIT 1",
     {},
     101,
     true,
     BuildSecEdgarQ24Probe}};

} // namespace q24_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct SecEdgarQ24Scan : SecEdgarQ24InitialScan {
  explicit SecEdgarQ24Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNum(
      const std::function<void(const SecEdgarQ24NumInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version, uom, value, ddate FROM num",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto value = result.GetValue(4, row);
                const auto date = result.GetValue(5, row);
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString(),
                          result.GetValue(3, row).ToString(),
                          value.IsNull() ? 0.0 : std::stod(value.ToString()),
                          value.IsNull(),
                          static_cast<int32_t>(SafeInt32(date))});
              });
  }
  void ScanPre(
      const std::function<void(const SecEdgarQ24PreInput &)> &consumer) override {
    ScanQuery(connection, "SELECT adsh, tag, version FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({result.GetValue(0, row).ToString(),
                          result.GetValue(1, row).ToString(),
                          result.GetValue(2, row).ToString()});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ24Layout::kAccelerator,
    "accel_sec_edgar_q24_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ24Config c{101, 102};
      return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24Module>(connection, CaptureTableList{}, c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ24Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ24Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ24Config c{101, 102};
      return MakeBinding<SecEdgarQ24Scan, SecEdgarQ24ParallelModule>(connection, CaptureTableList{}, c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
