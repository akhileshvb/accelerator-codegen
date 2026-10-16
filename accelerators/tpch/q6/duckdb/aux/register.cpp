// GENERATED QUERY: TPC-H Q6 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q6_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q6/duckdb/aux/module.hpp"
#include "tpch/q6/cost_model.hpp"
#include "tpch/q6/layout.hpp"
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
namespace q6_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ6Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(5, row))),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{6,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ6Layout::kAccelerator,
     "SELECT SUM(l_extendedprice * l_discount) AS revenue "
     "FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
     "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25",
     "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
     "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)",
     "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_quantity, "
     "l_shipdate FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
     "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25 "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))}),
       core::AuxValue::Of(core::AuxType::Double, 0.03),
       core::AuxValue::Of(core::AuxType::Double, 25.0)}},
     1,
     false,
     BuildTpchQ6Probe,
     core::kTpchQ6CostModel,
     "SELECT SUM(l_extendedprice * l_discount) AS revenue "
     "FROM lineitem "
     "WHERE l_shipdate >= $1 AND l_shipdate < $1 + INTERVAL '1' year "
     "AND l_discount BETWEEN $2 - 0.01 AND $2 + 0.01 AND l_quantity < $3"}};

} // namespace q6_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ6Scan : TpchQ6InitialScan {
  explicit TpchQ6Scan(::duckdb::Connection &connection) : connection(connection) {}
  // (shipdate, discount, quantity, price) as an input. A NULL shipdate never
  // qualifies and is skipped; a NULL discount or quantity is flagged, so its
  // month declines. A NULL price adds nothing (SUM skips it).
  static void ConsumeRow(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row,
                         const std::function<void(const TpchQ6LineitemInput &)> &consumer) {
    const auto shipdate = ChunkValue(chunk, 0, row);
    if (shipdate.IsNull())
      return;
    const auto discount = ChunkValue(chunk, 1, row);
    const auto quantity = ChunkValue(chunk, 2, row);
    TpchQ6LineitemInput input{DateDays(shipdate), DecimalRaw(discount), 2,
                              DecimalRaw(quantity), 2,
                              DecimalRaw(ChunkValue(chunk, 3, row)), 2};
    input.null_cell = discount.IsNull() || quantity.IsNull();
    consumer(input);
  }
  void ScanLineitems(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          ConsumeRow(chunk, row, consumer);
        });
  }
  void ScanLineitemGroups(
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT date_trunc('month', l_shipdate)::DATE, l_discount, l_quantity, "
        "SUM(l_extendedprice)::DECIMAL(18,2) FROM lineitem "
        "GROUP BY date_trunc('month', l_shipdate), l_discount, l_quantity",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          ConsumeRow(chunk, row, consumer);
        });
  }
  void ScanLineitemsForMonths(
      const std::set<int32_t> &month_keys,
      const std::function<void(const TpchQ6LineitemInput &)> &consumer) override {
    if (month_keys.empty())
      return;
    std::ostringstream sql;
    sql << "SELECT date_trunc('month', l_shipdate)::DATE, l_discount, l_quantity, "
           "SUM(l_extendedprice)::DECIMAL(18,2) "
           "FROM lineitem WHERE (EXTRACT(year FROM l_shipdate)::INT * 12 + "
           "EXTRACT(month FROM l_shipdate)::INT - 1) IN (";
    bool first = true;
    for (const auto key : month_keys) {
      if (!first)
        sql << ", ";
      first = false;
      sql << key;
    }
    sql << ") GROUP BY date_trunc('month', l_shipdate), l_discount, l_quantity";
    ScanQueryStreaming(
        connection, sql.str(),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          ConsumeRow(chunk, row, consumer);
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ6Layout::kAccelerator,
    "accel_tpch_q6_aux",
    {::duckdb::LogicalType::DATE, ::duckdb::LogicalType::DOUBLE, ::duckdb::LogicalType::DOUBLE},
    [](::duckdb::Connection &connection) {
      const TpchQ6Config c{1};
      return MakeBinding<TpchQ6Scan, TpchQ6Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
