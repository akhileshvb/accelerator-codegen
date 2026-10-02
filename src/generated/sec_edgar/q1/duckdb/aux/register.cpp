// GENERATED QUERY: SEC EDGAR Q1 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q1_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q1/duckdb/aux/module.hpp"
#include "sec_edgar/q1/duckdb/aux/parallel/module.hpp"
#include "sec_edgar/q1/layout.hpp"
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
namespace q1_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildSecEdgarQ1Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const auto line = result.GetValue(1, row).ToString();
  const auto stmt = result.GetValue(2, row).ToString();
  const auto rfile = result.GetValue(3, row).ToString();
  const int64_t original = SafeInt64(result.GetValue(1, row));
  const auto stmt_sql = SqlLiteral(stmt);
  const auto rfile_sql = SqlLiteral(rfile);
  const auto adsh_sql = SqlLiteral(adsh);
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(original);
  probe.old_row = {{Text(result.GetValue(2, row)), Text(result.GetValue(3, row)),
                    Text(result.GetValue(0, row)), Int(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Int(original + 1);
  probe.update_sql = "UPDATE pre SET line = line + 1 WHERE adsh = " + adsh_sql +
                     " AND line = " + line + " AND stmt = " + stmt_sql +
                     " AND rfile = " + rfile_sql;
  probe.restore_sql = "UPDATE pre SET line = " + std::to_string(original) +
                      " WHERE adsh = " + adsh_sql + " AND line = " +
                      std::to_string(original + 1) + " AND stmt = " + stmt_sql +
                      " AND rfile = " + rfile_sql;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{1,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ1Layout::kAccelerator,
     "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, "
     "AVG(line) AS avg_line_num "
     "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC",
     "SELECT * FROM accel_sec_edgar_q1_aux()",
     "SELECT * FROM accel_sec_edgar_q1()",
     "SELECT * FROM accel_sec_edgar_q1_aux()",
     "SELECT adsh, line, stmt, rfile FROM pre WHERE stmt IS NOT NULL "
     "ORDER BY adsh, line LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ1Probe}};

} // namespace q1_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct SecEdgarQ1Scan : SecEdgarQ1InitialScan {
  explicit SecEdgarQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanPre(
      const std::function<void(const SecEdgarQ1PreInput &)> &consumer) const override {
    ScanQuery(connection, "SELECT stmt, rfile, adsh, line FROM pre",
              [&](auto &result, ::duckdb::idx_t row) {
                const auto stmt = result.GetValue(0, row);
                const auto rfile = result.GetValue(1, row);
                consumer({stmt.IsNull() ? std::string() : stmt.ToString(),
                          rfile.IsNull() ? std::string() : rfile.ToString(),
                          rfile.IsNull(), stmt.IsNull(),
                          result.GetValue(2, row).ToString(),
                          static_cast<int32_t>(SafeInt32(result.GetValue(3, row)))});
              });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::SecEdgarQ1Layout::kAccelerator,
    "accel_sec_edgar_q1_aux",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ1Config c{1};
      return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1Module>(connection, CaptureTableList{}, c);
    }}};

#ifdef ACCEL_ENABLE_PARALLEL_AUX
const AuxQueryRegistrar kParallelQuery{AuxQuery{
    std::string(core::SecEdgarQ1Layout::kAccelerator) + "_aux_parallel",
    std::string("accel_") + core::SecEdgarQ1Layout::kAccelerator + "_aux_parallel",
    {},
    [](::duckdb::Connection &connection) {
      const SecEdgarQ1Config c{1};
      return MakeBinding<SecEdgarQ1Scan, SecEdgarQ1ParallelModule>(connection, CaptureTableList{}, c);
    }}};
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
