// GENERATED QUERY: TPC-H Q3 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q3_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q3/duckdb/aux/module.hpp"
#include "tpch/q3/layout.hpp"
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

SharedProbe BuildTpchQ3Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 3;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Date(DateDays(result.GetValue(4, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{3,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ3Layout::kAccelerator,
     "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
     "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
     "o_orderdate, o_shippriority "
     "FROM customer, orders, lineitem "
     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
     "l_orderkey = o_orderkey "
     "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority "
     "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)",
     "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
     "SELECT * FROM accel_tpch_q3('BUILDING', DATE '1995-03-15')",
     "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
     // Probe rows that participate in the TOP-10 so a price bump changes revenue.
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate "
     "FROM lineitem WHERE l_orderkey IN ("
     "SELECT l_orderkey FROM ("
     "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
     "o_orderdate, o_shippriority "
     "FROM customer, orders, lineitem "
     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
     "l_orderkey = o_orderkey AND o_orderdate < DATE '1995-03-15' AND "
     "l_shipdate > DATE '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority "
     "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10))"
     "AND l_shipdate > DATE '1995-03-15' ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"BUILDING"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-03-15"))})}},
     3,
     false,
     BuildTpchQ3Probe}};

} // namespace q3_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ3Scan : TpchQ3InitialScan {
  explicit TpchQ3Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ3CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_mktsegment FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ3OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt32(ChunkValue(chunk, 3, row))});
        });
  }
  bool LookupOrderContext(int64_t orderkey, OrderContext &out) override {
    out = {};
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT o.o_orderkey, o.o_orderdate, o.o_custkey, o.o_shippriority, "
        "c.c_mktsegment "
        "FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey "
        "WHERE o.o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out.orderdate_days = DateDays(ChunkValue(chunk, 1, row));
          out.custkey = SafeInt64(ChunkValue(chunk, 2, row));
          out.shippriority = SafeInt32(ChunkValue(chunk, 3, row));
          out.segment = TpchQ3PartitionFromMktsegment(
              ChunkValue(chunk, 4, row).ToString());
          found = out.segment != 0;
        });
    return found;
  }
  void ScanLineitems(
      const std::function<void(const TpchQ3LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DateDays(ChunkValue(chunk, 3, row))});
        });
  }
  bool ScanSegmentJoin(
      const std::string &mktsegment,
      const std::function<void(const TpchQ3OrderInput &,
                               const TpchQ3LineitemInput &)> &consumer) override {
    if (mktsegment.empty())
      return true;
    std::ostringstream sql;
    sql << "SELECT o.o_orderkey, o.o_orderdate, o.o_custkey, o.o_shippriority, "
           "l.l_extendedprice, l.l_discount, l.l_shipdate "
           "FROM lineitem l "
           "JOIN orders o ON l.l_orderkey = o.o_orderkey "
           "JOIN customer c ON o.o_custkey = c.c_custkey "
           "WHERE c.c_mktsegment = '"
        << mktsegment << "'";
    ScanQueryStreaming(
        connection, sql.str(),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt32(ChunkValue(chunk, 3, row))},
                   {SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 5, row)), 2,
                    DateDays(ChunkValue(chunk, 6, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ3Layout::kAccelerator,
    "accel_tpch_q3_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ3Config c{1, 2, 3};
      return MakeBinding<TpchQ3Scan, TpchQ3Module>(connection, CaptureTableList{{"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
