// GENERATED QUERY: TPC-H Q9 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q9_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q9/duckdb/aux/module.hpp"
#include "tpch/q9/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

constexpr accelerator_codegen::aux_state::query::TpchQ9Config kTables{1, 2, 3, 4, 5, 6};

namespace q9_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ9Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Aux row layout matches TpchQ9 Dependencies lineitem cols:
  // partkey, suppkey, orderkey, quantity, extendedprice, discount.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(5, row).ToString());
  SharedProbe probe;
  probe.table = kTables.lineitem_table;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(2, row)), Int(result.GetValue(3, row)),
                    Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(6, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[4] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(5, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{9,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ9Layout::kAccelerator,
     "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
     "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
     "FROM part, supplier, lineitem, partsupp, orders, nation "
     "WHERE s_suppkey = l_suppkey "
     "AND ps_suppkey = l_suppkey "
     "AND ps_partkey = l_partkey "
     "AND p_partkey = l_partkey "
     "AND o_orderkey = l_orderkey "
     "AND s_nationkey = n_nationkey "
     "AND p_name LIKE '%green%'"
     ") AS profit "
     "GROUP BY nation, o_year "
     "ORDER BY nation, o_year DESC",
     "SELECT * FROM accel_tpch_q9_aux('green')",
     "SELECT * FROM accel_tpch_q9('green')",
     "SELECT * FROM accel_tpch_q9_aux('green')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_suppkey, l_quantity, "
     "l_extendedprice, l_discount FROM lineitem "
     "WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '%green%') "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"green"})}},
     6,
     false,
     BuildTpchQ9Probe,
     core::kTpchQ9CostModel,
     "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ( "
     "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
     "FROM part, supplier, lineitem, partsupp, orders, nation "
     "WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey "
     "AND ps_partkey = l_partkey AND p_partkey = l_partkey "
     "AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey "
     "AND p_name LIKE '%' || $1 || '%') AS profit "
     "GROUP BY nation, o_year ORDER BY nation, o_year DESC"}};

} // namespace q9_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ9Scan : TpchQ9InitialScan {
  explicit TpchQ9Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9PartInput &)> &sink) override {
    ScanPartitioned(services, connection, "part",
                    {IntColumn("p_partkey"), TextColumn("p_name")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ9PartInput{row.Int(0), row.Text(1)});
                      }
                    });
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9SupplierInput &)> &sink) override {
    ScanPartitioned(services, connection, "supplier",
                    {IntColumn("s_suppkey"), IntColumn("s_nationkey")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ9SupplierInput{row.Int(0), static_cast<int32_t>(row.Int(1))});
                      }
                    });
  }
  void ScanNations(
      const std::function<void(const TpchQ9NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9OrdersInput &)> &sink) override {
    ScanPartitioned(services, connection, "orders",
                    {IntColumn("o_orderkey"), DateColumn("o_orderdate")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ9OrdersInput{row.Int(0), row.Date(1)});
                      }
                    });
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9PartsuppInput &)> &sink) override {
    ScanPartitioned(services, connection, "partsupp",
                    {IntColumn("ps_partkey"), IntColumn("ps_suppkey"), DecimalColumn("ps_supplycost", 2)},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ9PartsuppInput{row.Int(0), row.Int(1), row.Decimal(2)});
                      }
                    });
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ9LineitemInput &)> &sink) override {
    ScanPartitioned(services, connection, "lineitem",
                    {IntColumn("l_partkey"), IntColumn("l_suppkey"), IntColumn("l_orderkey"),
                     DecimalColumn("l_quantity", 2), DecimalColumn("l_extendedprice", 2),
                     DecimalColumn("l_discount", 2)},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition,
                             TpchQ9LineitemInput{row.Int(0), row.Int(1), row.Int(2),
                                                 row.Decimal(3), row.Decimal(4), row.Decimal(5)});
                      }
                    });
  }
  bool LookupOrderYear(int64_t orderkey, int32_t &year) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT year(o_orderdate) FROM orders WHERE o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          year = SafeInt32(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ9Layout::kAccelerator,
    "accel_tpch_q9_aux",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      return MakeBinding<TpchQ9Scan, TpchQ9Module>(
          connection,
          CaptureTableList{{"part", kTables.part_table},
                           {"supplier", kTables.supplier_table},
                           {"nation", kTables.nation_table},
                           {"orders", kTables.orders_table},
                           {"partsupp", kTables.partsupp_table},
                           {"lineitem", kTables.lineitem_table}},
          kTables);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
