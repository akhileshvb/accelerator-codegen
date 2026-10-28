// GENERATED QUERY: TPC-H Q5 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q5_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q5/duckdb/aux/module.hpp"
#include "tpch/q5/cost_model.hpp"
#include "tpch/q5/layout.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <set>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q5_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

constexpr query::TpchQ5Config kTables{1, 2, 3, 4, 5, 6};

SharedProbe BuildTpchQ5Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Projection: orderkey, suppkey, price, discount, linenumber.
  // ReadLineitem expects (orderkey, suppkey, price, discount); linenumber is
  // only for the UPDATE WHERE clause.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(4, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = kTables.lineitem_table;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{5,
     "tpch",
     core::TpchQ5Layout::kAccelerator,
     "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM customer, orders, lineitem, supplier, nation, region "
     "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
     "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'AMERICA' "
     "AND o_orderdate >= DATE '1995-01-01' "
     "AND o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
     "GROUP BY n_name ORDER BY revenue DESC",
     "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q5('AMERICA', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
     "SELECT l.l_orderkey, l.l_suppkey, l.l_extendedprice, l.l_discount, "
     "l.l_linenumber "
     "FROM lineitem l "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "JOIN supplier s ON s.s_suppkey = l.l_suppkey AND s.s_nationkey = c.c_nationkey "
     "JOIN nation n ON n.n_nationkey = s.s_nationkey "
     "JOIN region r ON r.r_regionkey = n.n_regionkey "
     "WHERE r.r_name = 'AMERICA' "
     "AND o.o_orderdate >= DATE '1995-01-01' "
     "AND o.o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"AMERICA"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))})}},
     6,
     false,
     BuildTpchQ5Probe,
     core::kTpchQ5CostModel,
     "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM customer, orders, lineitem, supplier, nation, region "
     "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
     "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = $1 AND o_orderdate >= $2 AND o_orderdate < $2 + INTERVAL '1' year "
     "GROUP BY n_name ORDER BY revenue DESC"}};

} // namespace q5_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ5Scan : TpchQ5InitialScan {
  explicit TpchQ5Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanRegionsSerial(
      const std::function<void(const TpchQ5RegionInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT r_regionkey, r_name FROM region",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanNationsSerial(
      const std::function<void(const TpchQ5NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  bool ScanLineitemsBySupplier(
      int64_t suppkey,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem "
        "WHERE l_suppkey = " + std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2});
        });
    return true;
  }
  bool ScanLineitemsByOrder(
      int64_t orderkey,
      const std::function<void(const TpchQ5LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem "
        "WHERE l_orderkey = " + std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2});
        });
    return true;
  }
  void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                 const std::vector<BuildColumn> &columns, std::size_t partitions,
                 const Sink &sink) override {
    ScanPartitioned(services, connection, table, columns, partitions, sink);
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ5Layout::kAccelerator,
    "accel_tpch_q5_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ5Config c = q5_catalog::kTables;
      return MakeBinding<TpchQ5Scan, TpchQ5Module>(connection, CaptureTableList{{"region", c.region_table}, {"nation", c.nation_table}, {"customer", c.customer_table}, {"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
