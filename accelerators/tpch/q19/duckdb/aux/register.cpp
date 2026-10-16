// GENERATED QUERY: TPC-H Q19 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q19_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q19/duckdb/aux/module.hpp"
#include "tpch/q19/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "backends/duckdb/engine/decimal128.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {
namespace q19_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ19Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Select columns: orderkey, partkey, linenumber, qty, price, discount,
  // shipmode, shipinstruct. table=5 matches TpchQ19Config{5,6}.lineitem_table.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto partkey = result.GetValue(1, row).ToString();
  const auto linenumber = result.GetValue(2, row).ToString();
  double quantity = 0.0;
  try {
    quantity = std::stod(result.GetValue(3, row).ToString());
  } catch (const std::exception &) {
    quantity = static_cast<double>(DecimalRaw(result.GetValue(3, row))) / 100.0;
  }
  SharedProbe probe;
  probe.table = 5;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 100.0;
  probe.current_value = quantity;
  probe.old_row = {{Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Text(result.GetValue(6, row)), Text(result.GetValue(7, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(3, row)) + 10000), 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100"
                     " WHERE l_orderkey = " + orderkey +
                     " AND l_partkey = " + partkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(quantity) + " WHERE l_orderkey = " +
                      orderkey + " AND l_partkey = " + partkey +
                      " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{19,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ19Layout::kAccelerator,
     "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM lineitem, part "
     "WHERE (p_partkey = l_partkey "
     "AND p_brand = 'Brand#31' "
     "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
     "AND p_size BETWEEN 1 AND 5 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#12' "
     "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
     "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
     "AND p_size BETWEEN 1 AND 10 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#51' "
     "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
     "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
     "AND p_size BETWEEN 1 AND 15 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON')",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity, "
     "l.l_extendedprice, l.l_discount, l.l_shipmode, l.l_shipinstruct "
     "FROM lineitem l, part p "
     "WHERE p.p_partkey = l.l_partkey AND p.p_brand = 'Brand#31' "
     "AND p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l.l_quantity >= 4 AND l.l_quantity <= 14 "
     "AND p.p_size BETWEEN 1 AND 5 "
     "AND l.l_shipmode IN ('AIR', 'AIR REG') "
     "AND l.l_shipinstruct = 'DELIVER IN PERSON' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#31"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{4}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#12"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{15}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#51"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{23})}},
     5,
     false,
     BuildTpchQ19Probe,
     core::kTpchQ19CostModel}};

} // namespace q19_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ19Scan : TpchQ19InitialScan {
  explicit TpchQ19Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ19PartInput &)> &sink) override {
    ScanPartitioned(services, connection, "part",
                    {IntColumn("p_partkey"), TextColumn("p_brand"), TextColumn("p_container"),
                     IntColumn("p_size")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        const auto size = row.Int(3);
                        if (size != static_cast<int32_t>(size))
                          throw std::runtime_error("TPC-H Q19: p_size exceeds 32 bits");
                        sink(partition, TpchQ19PartInput{row.Int(0), row.Text(1), row.Text(2),
                                                         static_cast<int32_t>(size)});
                      }
                    });
  }
  void ScanGroupAggregates(
      const std::function<void(const TpchQ19GroupAggregate &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT p.b, p.f, " + QuantityRaw("l.l_quantity") + ", " +
            RevenueSum("l.l_extendedprice", "l.l_discount") +
            ", COUNT(*) FROM lineitem l JOIN (SELECT p_partkey, p_brand AS b, " +
            TpchQ19FamilySql() +
            " AS f FROM part) p ON l.l_partkey = p.p_partkey WHERE p.f IS NOT NULL AND " +
            TpchQ19ShippingSql() + " GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(TpchQ19GroupAggregate{
              ChunkValue(chunk, 0, row).ToString(),
              static_cast<int>(SafeInt64(ChunkValue(chunk, 1, row))),
              SafeInt64(ChunkValue(chunk, 2, row)),
              HugeintToInt128(ChunkValue(chunk, 3, row).GetValue<::duckdb::hugeint_t>()),
              SafeInt64(ChunkValue(chunk, 4, row))});
        });
  }
  void ScanPartQuantityRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const TpchQ19PartQuantityRevenue &)> &consumer) override {
    std::string keys;
    for (const auto key : partkeys)
      keys += (keys.empty() ? "" : ",") + std::to_string(key);
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, " + QuantityRaw("l_quantity") + ", " +
            RevenueSum("l_extendedprice", "l_discount") +
            ", COUNT(*) FROM lineitem WHERE l_partkey IN (" + keys + ") AND " +
            TpchQ19ShippingSql() + " GROUP BY 1, 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(TpchQ19PartQuantityRevenue{
              SafeInt64(ChunkValue(chunk, 0, row)), SafeInt64(ChunkValue(chunk, 1, row)),
              HugeintToInt128(ChunkValue(chunk, 2, row).GetValue<::duckdb::hugeint_t>()),
              SafeInt64(ChunkValue(chunk, 3, row))});
        });
  }

private:
  // Scale-2 decimal column as its unscaled BIGINT.
  static std::string QuantityRaw(const std::string &column) {
    return "CAST(round(" + column + " * 100) AS BIGINT)";
  }
  // SUM(price * (1 - discount)) at scale 4 as HUGEINT.
  static std::string RevenueSum(const std::string &price, const std::string &discount) {
    return "CAST(SUM(CAST(round(" + price + " * 100) AS HUGEINT) * (100 - CAST(round(" +
           discount + " * 100) AS HUGEINT))) AS HUGEINT)";
  }

  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ19Layout::kAccelerator,
    "accel_tpch_q19_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::BIGINT},
    [](::duckdb::Connection &connection) {
      const TpchQ19Config c{5, 6};
      return MakeBinding<TpchQ19Scan, TpchQ19Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}, {"part", c.part_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
