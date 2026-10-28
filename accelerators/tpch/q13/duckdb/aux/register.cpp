// GENERATED QUERY: TPC-H Q13 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q13_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q13/duckdb/aux/module.hpp"
#include "tpch/q13/cost_model.hpp"
#include "tpch/q13/layout.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q13_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

constexpr core::TableId kCustomerTable = 0;
constexpr core::TableId kOrdersTable = 1;

SharedProbe BuildTpchQ13Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto comment = result.GetValue(2, row).ToString();
  SharedProbe probe;
  probe.table = kOrdersTable;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(1, row)), Text(result.GetValue(2, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::String, comment + " unusual accounts");
  probe.update_sql =
      "UPDATE orders SET o_comment = o_comment || ' unusual accounts' "
      "WHERE o_orderkey = " + orderkey;
  probe.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(comment) +
                      " WHERE o_orderkey = " + orderkey;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{13,
     "tpch",
     core::TpchQ13Layout::kAccelerator,
     "SELECT c_count, COUNT(*) AS custdist FROM ("
     "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
     "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
     "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
     "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT o_orderkey, o_custkey, o_comment FROM orders "
     "WHERE o_comment NOT LIKE '%unusual%accounts%' "
     "ORDER BY o_orderkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String,
                          std::string{"%unusual%accounts%"})}},
     1,
     false,
     BuildTpchQ13Probe,
     core::kTpchQ13CostModel,
     "SELECT c_count, COUNT(*) AS custdist FROM ( "
     "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
     "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE $1 "
     "GROUP BY c_custkey) AS c_orders "
     "GROUP BY c_count ORDER BY custdist DESC, c_count DESC"}};

} // namespace q13_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ13Scan : TpchQ13InitialScan {
  explicit TpchQ13Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ13CustomerInput &)> &sink) override {
    ScanPartitioned(services, connection, "customer", {IntColumn("c_custkey")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i)
                        sink(partition, TpchQ13CustomerInput{batch.Row(i).Int(0)});
                    });
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, int64_t, std::string_view)> &sink) override {
    ScanPartitioned(services, connection, "orders",
                    {IntColumn("o_custkey"), TextColumn("o_comment")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, row.Int(0), row.TextView(1));
                      }
                    });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ13Layout::kAccelerator,
    "accel_tpch_q13_aux",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ13Config c{q13_catalog::kCustomerTable, q13_catalog::kOrdersTable};
      return MakeBinding<TpchQ13Scan, TpchQ13Module>(connection, CaptureTableList{{"customer", c.customer_table}, {"orders", c.orders_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
