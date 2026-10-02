// GENERATED QUERY: TPC-H Q4 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q4_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q4/duckdb/aux/module.hpp"
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
namespace q4_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ4Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Clear late-receipt on a sole Q4-qualifying lineitem so EXISTS flips false
  // for that order (price bumps never change Q4). Row layout matches
  // TpchQ4Module::ReadLineitem: orderkey, commitdate, receiptdate.
  // table id must match TpchQ4Config{1, 2}.lineitem_table.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int32_t commit_days = DateDays(result.GetValue(2, row));
  const int32_t receipt_days = DateDays(result.GetValue(3, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(receipt_days);
  probe.old_row = {{Int(result.GetValue(0, row)), Date(commit_days),
                    Date(receipt_days)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Date(commit_days);
  const auto commit_date =
      ::duckdb::Date::ToString(::duckdb::date_t{commit_days});
  const auto receipt_date =
      ::duckdb::Date::ToString(::duckdb::date_t{receipt_days});
  probe.update_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + commit_date +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + receipt_date +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
  probe.toggles = true;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{4,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ4Layout::kAccelerator,
     "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
     "FROM orders "
     "WHERE o_orderdate >= DATE '1993-07-01' "
     "AND o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
     "AND EXISTS ("
     "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
     "AND l_commitdate < l_receiptdate) "
     "GROUP BY o_orderpriority ORDER BY o_orderpriority",
     "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
     "SELECT * FROM accel_tpch_q4(DATE '1993-07-01')",
     "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
     // Sole late-receipt lineitem on an in-window order so clearing receipt
     // drops the order from EXISTS / order_count.
     "SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate, l.l_receiptdate "
     "FROM lineitem l "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "WHERE o.o_orderdate >= DATE '1993-07-01' "
     "AND o.o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
     "AND l.l_commitdate < l.l_receiptdate "
     "AND (SELECT COUNT(*) FROM lineitem l2 "
     "WHERE l2.l_orderkey = l.l_orderkey "
     "AND l2.l_commitdate < l2.l_receiptdate) = 1 "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1993-07-01"))})}},
     2,
     false,
     BuildTpchQ4Probe}};

} // namespace q4_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ4Scan : TpchQ4InitialScan {
  explicit TpchQ4Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanOrders(
      const std::function<void(const TpchQ4OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_orderdate, o_orderpriority FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    ChunkValue(chunk, 2, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ4LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_commitdate, l_receiptdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row))});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ4Layout::kAccelerator,
    "accel_tpch_q4_aux",
    {::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ4Config c{1, 2};
      return MakeBinding<TpchQ4Scan, TpchQ4Module>(connection, CaptureTableList{{"orders", c.orders_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
