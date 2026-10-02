// GENERATED QUERY: TPC-H Q1 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q1_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q1/duckdb/aux/module.hpp"
#include "tpch/q1/layout.hpp"
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

SharedProbe BuildTpchQ1Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(4, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(2, row))),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(6, row)), 2),
                    Text(result.GetValue(7, row)), Text(result.GetValue(8, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(4, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{1,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ1Layout::kAccelerator,
     "SELECT l_returnflag, l_linestatus, "
     "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
     "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
     "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
     "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
     "avg(l_discount) AS avg_disc, count(*) AS count_order "
     "FROM lineitem "
     "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
     "GROUP BY l_returnflag, l_linestatus "
     "ORDER BY l_returnflag, l_linestatus",
     "SELECT * FROM accel_tpch_q1_aux(90)",
     "SELECT * FROM accel_tpch_q1(90)",
     "SELECT * FROM accel_tpch_q1_aux(90)",
     "SELECT l_orderkey, l_linenumber, l_shipdate, l_quantity, l_extendedprice, "
     "l_discount, l_tax, l_returnflag, l_linestatus FROM lineitem "
     "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, int64_t{90})}},
     1,
     false,
     BuildTpchQ1Probe}};

} // namespace q1_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ1Scan : TpchQ1InitialScan {
  explicit TpchQ1Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ1LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, "
        "l_tax, l_returnflag, l_linestatus FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 4, row)), 2,
                    FirstChar(ChunkValue(chunk, 5, row)),
                    FirstChar(ChunkValue(chunk, 6, row))});
        });
  }
  bool ScanDayGroupAggregates(
      const std::function<void(const TpchQ1DayGroupAggregate &)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_returnflag, l_linestatus, "
        "COUNT(*)::BIGINT, "
        "SUM(CAST(round(l_quantity * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "SUM(CAST(round(l_discount * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * (1 + l_tax) * "
        "1000000) AS BIGINT)) "
        "FROM lineitem GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    FirstChar(ChunkValue(chunk, 1, row)),
                    FirstChar(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 4, row)),
                    SafeInt64(ChunkValue(chunk, 5, row)),
                    SafeInt64(ChunkValue(chunk, 6, row)),
                    SafeInt64(ChunkValue(chunk, 7, row)),
                    SafeInt64(ChunkValue(chunk, 8, row))});
        });
    return true;
  }
  bool ScanDayGroupAggregatesForDays(
      const std::set<int32_t> &days,
      const std::function<void(const TpchQ1DayGroupAggregate &)> &consumer)
      override {
    if (days.empty())
      return true;
    const std::string sql =
        "SELECT l_shipdate, l_returnflag, l_linestatus, "
        "COUNT(*)::BIGINT, "
        "SUM(CAST(round(l_quantity * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "SUM(CAST(round(l_discount * 100) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)), "
        "SUM(CAST(round(l_extendedprice * (1 - l_discount) * (1 + l_tax) * "
        "1000000) AS BIGINT)) "
        "FROM lineitem WHERE " +
        SqlDateInPredicate("l_shipdate", days) + " GROUP BY 1, 2, 3";
    ScanQueryStreaming(
        connection, sql,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 0, row)),
                    FirstChar(ChunkValue(chunk, 1, row)),
                    FirstChar(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 4, row)),
                    SafeInt64(ChunkValue(chunk, 5, row)),
                    SafeInt64(ChunkValue(chunk, 6, row)),
                    SafeInt64(ChunkValue(chunk, 7, row)),
                    SafeInt64(ChunkValue(chunk, 8, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ1Layout::kAccelerator,
    "accel_tpch_q1_aux",
    {::duckdb::LogicalType::INTEGER},
    [](::duckdb::Connection &connection) {
      const TpchQ1Config c{1};
      return MakeBinding<TpchQ1Scan, TpchQ1Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
