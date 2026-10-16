// GENERATED QUERY: TPC-H Q20 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q20_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q20/duckdb/aux/module.hpp"
#include "tpch/q20/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
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
namespace q20_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ20Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double quantity = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;  // TpchQ20Config lineitem_table
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 100000.0;
  probe.current_value = quantity;
  probe.old_row = {{Date(DateDays(result.GetValue(3, row))),
                    Int(result.GetValue(4, row)), Int(result.GetValue(5, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 10000000), 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100000"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(quantity) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{20,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ20Layout::kAccelerator,
     "SELECT s_name, s_address "
     "FROM supplier, nation "
     "WHERE s_suppkey IN ("
     "SELECT ps_suppkey FROM partsupp, ("
     "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
     "FROM lineitem WHERE l_shipdate >= DATE '1997-01-01' "
     "AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
     "GROUP BY l_partkey, l_suppkey) agg_lineitem "
     "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
     "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
     "AND ps_availqty > agg_quantity) "
     "AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
     "ORDER BY s_name",
     "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT * FROM accel_tpch_q20(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity, l.l_shipdate, "
     "l.l_partkey, l.l_suppkey FROM lineitem l "
     "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE 'gainsboro%' "
     "JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey "
     "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
     "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA' "
     "WHERE l.l_shipdate >= DATE '1997-01-01' "
     "AND l.l_shipdate < DATE '1998-01-01' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1997-01-01"))}),
       core::AuxValue::Of(core::AuxType::String, std::string{"gainsboro%"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"RUSSIA"})}},
     1,
     false,
     BuildTpchQ20Probe,
     core::kTpchQ20CostModel}};

} // namespace q20_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ20Scan : TpchQ20InitialScan {
  explicit TpchQ20Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20LineitemInput &)> &sink) override {
    ScanPartitioned(services, connection, "lineitem",
                    {DateColumn("l_shipdate"), IntColumn("l_partkey"), IntColumn("l_suppkey"),
                     DecimalColumn("l_quantity", 2)},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ20LineitemInput{row.Date(0), row.Int(1), row.Int(2),
                                                             row.Decimal(3)});
                      }
                    });
  }
  void ScanPartNames(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, int64_t, std::string_view)> &sink) override {
    ScanPartitioned(services, connection, "part", {IntColumn("p_partkey"), TextColumn("p_name")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, row.Int(0), row.TextView(1));
                      }
                    });
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20PartsuppInput &)> &sink) override {
    ScanPartitioned(services, connection, "partsupp",
                    {IntColumn("ps_partkey"), IntColumn("ps_suppkey"), IntColumn("ps_availqty")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ20PartsuppInput{row.Int(0), row.Int(1), row.Int(2)});
                      }
                    });
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ20SupplierInput &)> &sink) override {
    ScanPartitioned(services, connection, "supplier",
                    {IntColumn("s_suppkey"), TextColumn("s_name"), TextColumn("s_address"),
                     IntColumn("s_nationkey")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        const auto nationkey = row.Int(3);
                        if (nationkey != static_cast<int32_t>(nationkey))
                          throw std::runtime_error("TPC-H Q20: s_nationkey exceeds 32 bits");
                        sink(partition, TpchQ20SupplierInput{row.Int(0), row.Text(1), row.Text(2),
                                                             static_cast<int32_t>(nationkey)});
                      }
                    });
  }
  void ScanNations(
      const std::function<void(const TpchQ20NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ20Layout::kAccelerator,
    "accel_tpch_q20_aux",
    {::duckdb::LogicalType::DATE, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ20Config c{1, 2, 3, 4, 5};
      return MakeBinding<TpchQ20Scan, TpchQ20Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}, {"part", c.part_table}, {"partsupp", c.partsupp_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
