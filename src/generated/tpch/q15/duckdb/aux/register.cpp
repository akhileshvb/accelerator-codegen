// GENERATED QUERY: TPC-H Q15 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q15_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q15/duckdb/aux/module.hpp"
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
namespace q15_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ15Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(4, row))),
                    Int(result.GetValue(5, row)),
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

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{15,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ15Layout::kAccelerator,
     "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
     "FROM supplier, ("
     "SELECT l_suppkey AS supplier_no, "
     "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
     "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey) revenue0 "
     "WHERE s_suppkey = supplier_no "
     "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
     "SELECT l_suppkey AS supplier_no, "
     "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
     "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey",
     "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
     "SELECT * FROM accel_tpch_q15(DATE '1994-11-01')",
     "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, "
     "l_suppkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "AND l_suppkey = (SELECT l_suppkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey "
     "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1) "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1994-11-01"))})}},
     1,
     false,
     BuildTpchQ15Probe}};

} // namespace q15_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ15Scan : TpchQ15InitialScan {
  explicit TpchQ15Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const std::function<void(const TpchQ15SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString(),
                    ChunkValue(chunk, 3, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ15LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    2,
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    2});
        });
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const TpchQ15LineitemInput &)> &consumer)
      override {
    if (end_days <= start_days)
      return;
    ScanQueryStreaming(
        connection,
        "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
        "WHERE l_shipdate >= " +
            SqlDateLiteral(start_days) + " AND l_shipdate < " +
            SqlDateLiteral(end_days),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({DateDays(ChunkValue(chunk, 3, row)),
                    SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    2,
                    DecimalRaw(ChunkValue(chunk, 2, row)),
                    2});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ15Layout::kAccelerator,
    "accel_tpch_q15_aux",
    {::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ15Config c{1, 2};
      return MakeBinding<TpchQ15Scan, TpchQ15Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}, {"supplier", c.supplier_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
