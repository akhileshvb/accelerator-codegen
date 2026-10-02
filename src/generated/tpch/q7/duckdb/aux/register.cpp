// GENERATED QUERY: TPC-H Q7 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q7_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q7/duckdb/aux/module.hpp"
#include "tpch/q7/layout.hpp"
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
namespace q7_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ7Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Full ReadLineitem layout: orderkey, suppkey, extendedprice, discount, shipdate.
  // (Short 3-cell probe shape is ambiguous when multiple lineitems share an orderkey.)
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const auto suppkey = result.GetValue(2, row);
  const double price = std::stod(result.GetValue(3, row).ToString());
  const int64_t price_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t discount_raw = DecimalRaw(result.GetValue(4, row));
  const int32_t shipdate_days = DateDays(result.GetValue(5, row));
  SharedProbe probe;
  // Must match TpchQ7Config{0,1,2,3,4}.lineitem_table.
  probe.table = 4;
  probe.kind = core::MutationKind::Update;
  // Column ids match full lineitem mutation layout (extendedprice=2, discount=3).
  probe.updated_columns = {2};
  constexpr int64_t kPriceBumpRaw = 10000; // +100.00 at scale 2
  probe.write_delta = static_cast<double>(kPriceBumpRaw) / 100.0;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)), Int(suppkey),
                    Decimal(price_raw, 2), Decimal(discount_raw, 2),
                    Date(shipdate_days)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(price_raw + kPriceBumpRaw, 2);
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + " +
      std::to_string(probe.write_delta) + " WHERE l_orderkey = " + orderkey +
      " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{7,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ7Layout::kAccelerator,
     "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
     "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, "
     "EXTRACT(year FROM l_shipdate) AS l_year, "
     "l_extendedprice * (1 - l_discount) AS volume "
     "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
     "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
     "AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey "
     "AND c_nationkey = n2.n_nationkey "
     "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
     "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
     "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping "
     "GROUP BY supp_nation, cust_nation, l_year "
     "ORDER BY supp_nation, cust_nation, l_year",
     "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
     "SELECT * FROM accel_tpch_q7('UNITED KINGDOM', 'BRAZIL')",
     "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
     // Only lineitems that contribute to the UK↔Brazil shipping volume.
     // Columns feed BuildTpchQ7Probe full ReadLineitem AuxRow layout.
     "SELECT l.l_orderkey, l.l_linenumber, l.l_suppkey, l.l_extendedprice, "
     "l.l_discount, l.l_shipdate "
     "FROM lineitem l, supplier s, orders o, customer c, nation n1, nation n2 "
     "WHERE s.s_suppkey = l.l_suppkey AND o.o_orderkey = l.l_orderkey "
     "AND c.c_custkey = o.o_custkey AND s.s_nationkey = n1.n_nationkey "
     "AND c.c_nationkey = n2.n_nationkey "
     "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
     "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
     "AND l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"UNITED KINGDOM"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"BRAZIL"})}},
     4,
     false,
     BuildTpchQ7Probe}};

} // namespace q7_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ7Scan : TpchQ7InitialScan {
  explicit TpchQ7Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanNations(
      const std::function<void(const TpchQ7NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ7SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanCustomers(
      const std::function<void(const TpchQ7CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_nationkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ7OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_custkey FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ7LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
        "FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row)), 2,
                    DecimalRaw(ChunkValue(chunk, 3, row)), 2,
                    DateDays(ChunkValue(chunk, 4, row))});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ7Layout::kAccelerator,
    "accel_tpch_q7_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ7Config c{0, 1, 2, 3, 4};
      return MakeBinding<TpchQ7Scan, TpchQ7Module>(connection, CaptureTableList{{"nation", c.nation_table}, {"supplier", c.supplier_table}, {"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
