// GENERATED QUERY: TPC-H Q18 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q18_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q18/duckdb/aux/module.hpp"
#include "tpch/q18/cost_model.hpp"
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
namespace q18_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ18Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Large quantity bump so SUM(l_quantity) / sum_qty in the answer moves.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(2, row));
  SharedProbe probe;
  probe.table = 0;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(quantity_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Decimal(quantity_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(quantity_raw + 100, 2); // +1.00
  probe.update_sql =
      "UPDATE lineitem SET l_quantity = l_quantity + 1.00 WHERE l_orderkey = " +
      orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql =
      "UPDATE lineitem SET l_quantity = " +
      std::to_string(static_cast<double>(quantity_raw) / 100.0) +
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{18,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ18Layout::kAccelerator,
     "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
     "SUM(l_quantity) AS sum_qty "
     "FROM customer, orders, lineitem "
     "WHERE o_orderkey IN ("
     "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
     "HAVING SUM(l_quantity) > 312) "
     "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
     "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
     "ORDER BY o_totalprice DESC, o_orderdate ASC",
     "SELECT * FROM accel_tpch_q18_aux(312)",
     "SELECT * FROM accel_tpch_q18(312)",
     "SELECT * FROM accel_tpch_q18_aux(312)",
     "SELECT l_orderkey, l_linenumber, l_quantity FROM lineitem "
     "WHERE l_orderkey IN ("
     "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
     "HAVING SUM(l_quantity) > 312) "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, int64_t{312})}},
     0,
     false,
     BuildTpchQ18Probe,
     core::kTpchQ18CostModel}};

} // namespace q18_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ18Scan : TpchQ18InitialScan {
  explicit TpchQ18Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanLineitems(
      const std::function<void(const TpchQ18LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT l_orderkey, l_quantity FROM lineitem",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row))});
        });
  }
  bool ScanOrderQuantityAggregates(
      const std::function<void(const TpchQ18OrderQuantityAggregate &)> &consumer)
      override {
    const std::string min_qty =
        std::to_string(query::TpchQ18State::kMinPersistQuantityRaw);
    ScanQueryStreaming(
        connection,
        "SELECT l_orderkey, "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) "
        "FROM lineitem GROUP BY 1 HAVING "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) > " +
            min_qty,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row))});
        });
    return true;
  }
  bool ScanSurvivorOrders(
      int64_t min_quantity_raw,
      const std::function<void(const TpchQ18SurvivorAggregate &)> &consumer)
      override {
    ScanQueryStreaming(
        connection,
        "SELECT a.k, a.s, o.o_custkey, o.o_orderdate, o.o_totalprice FROM ("
        "SELECT l_orderkey AS k, "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) AS s "
        "FROM lineitem GROUP BY l_orderkey HAVING "
        "CAST(SUM(CAST(round(l_quantity * 100) AS BIGINT)) AS BIGINT) > " +
            std::to_string(min_quantity_raw) +
        ") a JOIN orders o ON a.k = o.o_orderkey",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row))});
        });
    return true;
  }
  void ScanOrders(
      const std::function<void(const TpchQ18OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row))});
        });
  }
  bool LookupOrder(int64_t orderkey, TpchQ18OrderInput &out) override {
    out = {};
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders "
        "WHERE o_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 SafeInt64(ChunkValue(chunk, 1, row)),
                 DateDays(ChunkValue(chunk, 2, row)),
                 DecimalRaw(ChunkValue(chunk, 3, row))};
          found = true;
        });
    return found;
  }
  bool LookupOrderQuantitySum(int64_t orderkey, int64_t &sum_raw) override {
    sum_raw = 0;
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT COALESCE(SUM(CAST(round(l_quantity * 100) AS BIGINT)), 0) "
        "FROM lineitem WHERE l_orderkey = " +
            std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          sum_raw = SafeInt64(ChunkValue(chunk, 0, row));
          found = true;
        });
    return found;
  }
  bool LookupCustomerName(int64_t custkey, std::string &name) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT c_name FROM customer WHERE c_custkey = " +
            std::to_string(custkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          name = ChunkValue(chunk, 0, row).ToString();
          found = true;
        });
    return found;
  }
  void ScanCustomers(
      const std::function<void(const TpchQ18CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey, c_name FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ18Layout::kAccelerator,
    "accel_tpch_q18_aux",
    {::duckdb::LogicalType::BIGINT},
    [](::duckdb::Connection &connection) {
      const TpchQ18Config c{0, 1, 2};
      return MakeBinding<TpchQ18Scan, TpchQ18Module>(connection, CaptureTableList{{"lineitem", c.lineitem_table}, {"orders", c.orders_table}, {"customer", c.customer_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
