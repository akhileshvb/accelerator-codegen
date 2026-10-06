// GENERATED QUERY: TPC-H Q9 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q9_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q9/duckdb/aux/module.hpp"
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
  probe.table = 6;
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
     BuildTpchQ9Probe}};

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
      const std::function<void(const TpchQ9PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_name FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ9SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row))});
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
      const std::function<void(const TpchQ9OrdersInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_orderkey, o_orderdate FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DateDays(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanPartsupps(
      const std::function<void(const TpchQ9PartsuppInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ9LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, "
        "l_extendedprice, l_discount FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row)),
                    DecimalRaw(ChunkValue(chunk, 5, row))});
        });
  }
  bool ScanProfitContributions(
      const std::function<void(const TpchQ9ProfitContribution &)> &consumer)
      override {
    // Push LI ⨝ orders ⨝ supplier ⨝ partsupp profit aggregation into DuckDB.
    ScanQueryStreaming(
        connection,
        "SELECT l.l_partkey, s.s_nationkey, year(o.o_orderdate), "
        "SUM(CAST(round(l.l_extendedprice * (1 - l.l_discount) * 10000) AS "
        "BIGINT) - "
        "CAST(round(ps.ps_supplycost * l.l_quantity * 10000) AS BIGINT)) "
        "FROM lineitem l "
        "JOIN orders o ON l.l_orderkey = o.o_orderkey "
        "JOIN supplier s ON l.l_suppkey = s.s_suppkey "
        "JOIN partsupp ps ON l.l_partkey = ps.ps_partkey AND "
        "l.l_suppkey = ps.ps_suppkey "
        "GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt32(ChunkValue(chunk, 1, row)),
                    SafeInt32(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  bool ScanPairAggregates(
      const std::function<void(int64_t, int64_t, int32_t, int64_t, int64_t)>
          &consumer) override {
    // Per (part, supplier, order year) lineitem aggregates; dimension-side
    // joins (supplier/partsupp/nation) are applied in the accelerator so
    // dimension writes stay incremental. rev = SUM(ep_raw * (100 - disc_raw)),
    // qty = SUM(quantity_raw).
    ScanQueryStreaming(
        connection,
        "SELECT l.l_partkey, l.l_suppkey, year(o.o_orderdate), "
        "SUM(CAST(round(l.l_extendedprice * (1 - l.l_discount) * 10000) AS "
        "BIGINT)), "
        "SUM(CAST(round(l.l_quantity * 100) AS BIGINT)) "
        "FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey "
        "GROUP BY 1, 2, 3",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                   SafeInt64(ChunkValue(chunk, 1, row)),
                   SafeInt32(ChunkValue(chunk, 2, row)),
                   SafeInt64(ChunkValue(chunk, 3, row)),
                   SafeInt64(ChunkValue(chunk, 4, row)));
        });
    return true;
  }
  bool LookupSupplierNation(int64_t suppkey, int32_t &nationkey) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT s_nationkey FROM supplier WHERE s_suppkey = " +
            std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          nationkey = SafeInt32(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
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
  bool LookupPartsuppCost(int64_t partkey, int64_t suppkey,
                          int64_t &supplycost_raw) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT ps_supplycost FROM partsupp WHERE ps_partkey = " +
            std::to_string(partkey) + " AND ps_suppkey = " +
            std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          supplycost_raw = DecimalRaw(ChunkValue(chunk, 0, row));
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
      const TpchQ9Config c{1, 2, 3, 4, 5, 6};
      return MakeBinding<TpchQ9Scan, TpchQ9Module>(connection, CaptureTableList{{"part", c.part_table}, {"supplier", c.supplier_table}, {"nation", c.nation_table}, {"orders", c.orders_table}, {"partsupp", c.partsupp_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
