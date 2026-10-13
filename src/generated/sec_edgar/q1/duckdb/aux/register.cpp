// GENERATED QUERY: SEC EDGAR Q1 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_sec_edgar_q1_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "sec_edgar/q1/common/module.hpp"
#include "sec_edgar/q1/duckdb/aux/native_scan.hpp"
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

// Q1 probe: a *repeatable, additive* point update keyed by rowid.
// A probe keyed on the row's original values matches no row after its first
// application, and a +1 on `line` moves AVG(line) by ~3e-7, below the 6-digit
// result fingerprint. `line + 1000` is observable in avg_line_num, and
// old_row/new_row (a +1000 shift) describe the SQL effect exactly on every
// application.
SharedProbe BuildSecEdgarQ1Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  constexpr int64_t kLineShift = 1000;
  const auto rowid = result.GetValue(4, row).ToString();
  const int64_t original = SafeInt64(result.GetValue(1, row));
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = static_cast<double>(kLineShift);
  probe.current_value = static_cast<double>(original);
  probe.old_row = {{Text(result.GetValue(2, row)), Text(result.GetValue(3, row)),
                    Text(result.GetValue(0, row)), Int(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Int(original + kLineShift);
  probe.update_sql = "UPDATE pre SET line = line + " + std::to_string(kLineShift) +
                     " WHERE rowid = " + rowid;
  probe.restore_sql = "UPDATE pre SET line = " + std::to_string(original) +
                      " WHERE rowid = " + rowid;
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
     "SELECT adsh, line, stmt, rfile, rowid FROM pre WHERE stmt IS NOT NULL "
     "AND line IS NOT NULL LIMIT 1",
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
    ScanQueryStreaming(
        connection, "SELECT stmt, rfile, adsh, line FROM pre",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          const auto stmt = ChunkValue(chunk, 0, row);
          const auto rfile = ChunkValue(chunk, 1, row);
          const auto adsh = ChunkValue(chunk, 2, row);
          const auto line = ChunkValue(chunk, 3, row);
          SecEdgarQ1PreInput input;
          input.stmt_is_null = stmt.IsNull();
          input.stmt = stmt.IsNull() ? std::string() : stmt.ToString();
          input.rfile_is_null = rfile.IsNull();
          input.rfile = rfile.IsNull() ? std::string() : rfile.ToString();
          input.adsh_is_null = adsh.IsNull();
          input.adsh = adsh.IsNull() ? std::string() : adsh.ToString();
          input.line_is_null = line.IsNull();
          input.line = line.IsNull() ? 0 : static_cast<int32_t>(SafeInt32(line));
          consumer(input);
        });
  }
  bool ScanPreChunks(
      const std::function<void(::duckdb::DataChunk &)> &consumer) const override {
    return ScanSecEdgarQ1PreNative(connection, consumer);
  }
  bool ScanPreInto(q1::Builder &builder) const override {
    return ScanSecEdgarQ1PreNative(
        connection, [&](::duckdb::DataChunk &chunk) { builder.ConsumeChunk(chunk); });
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
