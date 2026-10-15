// GENERATED QUERY: TPC-H Q12 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q12_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q12/duckdb/aux/module.hpp"
#include "tpch/q12/cost_model.hpp"
#include "tpch/q12/layout.hpp"
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
namespace q12_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ12Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Q12 counts by shipmode — price updates never change the answer. Flip MAIL
  // <-> AIR (outside the queried {'MAIL','SHIP'} pair) so high/low counts move.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const std::string shipmode = result.GetValue(6, row).ToString();
  const std::string new_shipmode = (shipmode == "MAIL") ? "AIR" : "MAIL";
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1}; // shipmode slot in aux lineitem row
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Text(result.GetValue(6, row)),
                    Date(DateDays(result.GetValue(3, row))),
                    Date(DateDays(result.GetValue(4, row))),
                    Date(DateDays(result.GetValue(5, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::String, new_shipmode);
  probe.update_sql = "UPDATE lineitem SET l_shipmode = '" + new_shipmode +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_shipmode = '" + shipmode +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
  probe.toggles = true;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{12,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ12Layout::kAccelerator,
     "SELECT l_shipmode, "
     "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
     "THEN 1 ELSE 0 END) AS high_line_count, "
     "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
     "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
     "FROM orders, lineitem "
     "WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
     "AND l_receiptdate >= DATE '1995-01-01' "
     "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
     "GROUP BY l_shipmode ORDER BY l_shipmode",
     "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_commitdate, "
     "l_receiptdate, l_shipdate, l_shipmode FROM lineitem "
     "WHERE l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_receiptdate >= DATE '1995-01-01' "
     "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"MAIL"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"SHIP"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))})}},
     2,
     false,
     BuildTpchQ12Probe,
     core::kTpchQ12CostModel}};

} // namespace q12_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// Zone-map friendly: contiguous runs of days become BETWEEN ranges.
std::string ReceiptRunsPredicate(const std::set<int32_t> &days) {
  if (days.size() > 64)
    return "l_receiptdate BETWEEN " + SqlDateLiteral(*days.begin()) + " AND " +
           SqlDateLiteral(*days.rbegin());
  std::string out = "(";
  bool first = true;
  auto it = days.begin();
  while (it != days.end()) {
    const int32_t lo = *it;
    int32_t hi = lo;
    for (++it; it != days.end() && *it == hi + 1; ++it)
      hi = *it;
    if (!first)
      out += " OR ";
    first = false;
    out += "l_receiptdate BETWEEN " + SqlDateLiteral(lo) + " AND " +
           SqlDateLiteral(hi);
  }
  return out + ")";
}

struct TpchQ12Scan : TpchQ12InitialScan {
  explicit TpchQ12Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanOrders(
      const std::function<void(const TpchQ12OrderInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT o_orderkey, o_orderpriority FROM orders",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ12OrderInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                   ChunkValue(chunk, 1, row).ToString()};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ12LineitemInput &)> &consumer) override {
    ScanQueryDecoded(
        connection,
        "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, "
        "l_shipdate FROM lineitem",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ12LineitemInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                      ChunkValue(chunk, 1, row).ToString(),
                                      DateDays(ChunkValue(chunk, 2, row)),
                                      DateDays(ChunkValue(chunk, 3, row)),
                                      DateDays(ChunkValue(chunk, 4, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  bool LookupQualifyingLineitems(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(int64_t, int32_t, const std::string &)>
          &consumer) override {
    if (orderkeys.empty())
      return true;
    std::string sql =
        "SELECT l_orderkey, l_receiptdate, l_shipmode FROM lineitem WHERE "
        "l_orderkey IN (";
    for (std::size_t i = 0; i < orderkeys.size(); ++i) {
      if (i > 0)
        sql += ',';
      sql += std::to_string(orderkeys[i]);
    }
    sql += ") AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate";
    ScanQueryStreaming(
        connection, sql,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                   DateDays(ChunkValue(chunk, 1, row)),
                   ChunkValue(chunk, 2, row).ToString());
        });
    return true;
  }
  void ScanLineitemsForReceiptDays(
      const std::set<int32_t> &receipt_days,
      const std::function<void(const TpchQ12LineitemInput &)> &consumer)
      override {
    if (receipt_days.empty())
      return;
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, "
        "l_shipdate FROM lineitem WHERE " +
            ReceiptRunsPredicate(receipt_days),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row)),
                    DateDays(ChunkValue(chunk, 4, row))});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ12Layout::kAccelerator,
    "accel_tpch_q12_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ12Config c{1, 2};
      return MakeBinding<TpchQ12Scan, TpchQ12Module>(connection, CaptureTableList{{"orders", c.orders_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
