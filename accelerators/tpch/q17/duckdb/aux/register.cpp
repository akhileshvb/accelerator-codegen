// GENERATED QUERY: TPC-H Q17 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q17_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q17/duckdb/aux/module.hpp"
#include "tpch/q17/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>
#include <unordered_map>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q17_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

constexpr core::TableId kPartTable = 1;
constexpr core::TableId kLineitemTable = 2;

SharedProbe BuildTpchQ17Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Bump extendedprice on a row already in the Q17 brand/container filter so
  // avg_yearly moves (tiny quantity bumps can be invisible after /7.0).
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t price_raw = DecimalRaw(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = kLineitemTable;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(price_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(2, row)), Decimal(quantity_raw, 2),
                    Decimal(price_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(price_raw + 100, 2); // +1.00
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + 1.00 WHERE l_orderkey = " +
      orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql =
      "UPDATE lineitem SET l_extendedprice = " +
      std::to_string(static_cast<double>(price_raw) / 100.0) +
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{17,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ17Layout::kAccelerator,
     // DuckDB TPC-H uses abbreviated containers (LG CASE), not official
     // syllable pairs (LARGE BRUSHED). Defaults must match the loaded DB.
     "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
     "AND l_quantity < avg_quantity",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')",
     "SELECT * FROM accel_tpch_q17('Brand#34', 'LG CASE')",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_quantity, l_extendedprice "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
     "AND l_quantity < avg_quantity "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#34"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"LG CASE"})}},
     2,
     false,
     BuildTpchQ17Probe,
     core::kTpchQ17CostModel,
     "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = $1 AND p_container = $2 AND l_quantity < avg_quantity"}};

} // namespace q17_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ17Scan : TpchQ17InitialScan {
  explicit TpchQ17Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ17PartInput &)> &sink) override {
    ScanPartitioned(services, connection, "part",
                    {IntColumn("p_partkey"), TextColumn("p_brand"), TextColumn("p_container")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ17PartInput{row.Int(0), row.Text(1), row.Text(2)});
                      }
                    });
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ17LineitemInput &)> &sink) override {
    ScanPartitioned(services, connection, "lineitem",
                    {IntColumn("l_partkey"), DecimalColumn("l_quantity", 2),
                     DecimalColumn("l_extendedprice", 2)},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ17LineitemInput{row.Int(0), row.Decimal(1), row.Decimal(2)});
                      }
                    });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ17Layout::kAccelerator,
    "accel_tpch_q17_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ17Config c{q17_catalog::kPartTable, q17_catalog::kLineitemTable};
      return MakeBinding<TpchQ17Scan, TpchQ17Module>(connection, CaptureTableList{{"part", c.part_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
