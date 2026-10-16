// GENERATED QUERY: TPC-H Q8 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q8_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q8/duckdb/aux/module.hpp"
#include "tpch/q8/cost_model.hpp"
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
namespace q8_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ8Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // AuxRow matches TpchQ8 ReadLineitem: partkey, orderkey, suppkey, price, discount.
  // table id matches TpchQ8Config{0,1,2,3,4}.lineitem_table = 4.
  // The bump is the line's own price, so mkt_share moves past the fingerprint's
  // rounding at any money scale (a fixed +100.00 rounds away on scaled prices).
  const auto orderkey = result.GetValue(1, row).ToString();
  const auto linenumber = result.GetValue(5, row).ToString();
  const auto price_text = result.GetValue(3, row).ToString();
  const double price = std::stod(price_text);
  const int64_t price_raw = DecimalRaw(result.GetValue(3, row));
  SharedProbe probe;
  probe.table = 4;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = price;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(1, row)),
                    Int(result.GetValue(2, row)), Decimal(price_raw, 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(2 * price_raw, 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + " + price_text +
                     " WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{8,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ8Layout::kAccelerator,
     "SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / "
     "SUM(volume) AS mkt_share FROM ("
     "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
     "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
     "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
     "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
     "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
     "AND r_name = 'ASIA' AND s_nationkey = n2.n_nationkey "
     "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "AND p_type = 'PROMO BRUSHED BRASS') AS all_nations "
     "GROUP BY o_year ORDER BY o_year",
     "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     "SELECT * FROM accel_tpch_q8('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     // Must join the full Q8 path (ASIA customer region) so +price changes mkt_share.
     "SELECT l.l_partkey, l.l_orderkey, l.l_suppkey, l.l_extendedprice, "
     "l.l_discount, l.l_linenumber FROM lineitem l "
     "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_type = 'PROMO BRUSHED BRASS' "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "JOIN nation n1 ON n1.n_nationkey = c.c_nationkey "
     "JOIN region r ON r.r_regionkey = n1.n_regionkey AND r.r_name = 'ASIA' "
     "WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"INDIA"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"ASIA"}),
       core::AuxValue::Of(core::AuxType::String,
                          std::string{"PROMO BRUSHED BRASS"})}},
     4,
     false,
     BuildTpchQ8Probe,
     core::kTpchQ8CostModel,
     "SELECT o_year, SUM(CASE WHEN nation = $1 THEN volume ELSE 0 END) / SUM(volume) AS mkt_share FROM ( "
     "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
     "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
     "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
     "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
     "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
     "AND r_name = $2 AND s_nationkey = n2.n_nationkey "
     "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "AND p_type = $3) AS all_nations "
     "GROUP BY o_year ORDER BY o_year"}};

} // namespace q8_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ8Scan : TpchQ8InitialScan {
  explicit TpchQ8Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ8PartInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT p_partkey, p_type FROM part",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ8PartInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                 ChunkValue(chunk, 1, row).ToString()};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanSuppliers(
      const std::function<void(const TpchQ8SupplierInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT s_suppkey, s_nationkey FROM supplier",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ8SupplierInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                     SafeInt32(ChunkValue(chunk, 1, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanCustomers(
      const std::function<void(const TpchQ8CustomerInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT c_custkey, c_nationkey FROM customer",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ8CustomerInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                     SafeInt32(ChunkValue(chunk, 1, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanOrders(
      const std::function<void(const TpchQ8OrdersInput &)> &consumer) override {
    ScanQueryDecoded(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ8OrdersInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                   SafeInt64(ChunkValue(chunk, 1, row)),
                                   DateDays(ChunkValue(chunk, 2, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ8LineitemInput &)> &consumer) override {
    ScanQueryDecoded(
        connection,
        "SELECT l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount "
        "FROM lineitem",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ8LineitemInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                     SafeInt64(ChunkValue(chunk, 1, row)),
                                     SafeInt64(ChunkValue(chunk, 2, row)),
                                     DecimalRaw(ChunkValue(chunk, 3, row)),
                                     DecimalRaw(ChunkValue(chunk, 4, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanNations(
      const std::function<void(const TpchQ8NationInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT n_nationkey, n_name, n_regionkey FROM nation",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  void ScanRegions(
      const std::function<void(const TpchQ8RegionInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT r_regionkey, r_name FROM region",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt32(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  // Point lookups: same connection as the transaction, so its own writes are
  // visible (as for the other queries' Lookup* calls).
  bool LookupPart(int64_t partkey, TpchQ8PartInput &out) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT p_partkey, p_type FROM part WHERE p_partkey = " +
            std::to_string(partkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 ChunkValue(chunk, 1, row).ToString()};
          found = true;
        });
    return found;
  }
  bool LookupSupplier(int64_t suppkey, TpchQ8SupplierInput &out) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT s_suppkey, s_nationkey FROM supplier WHERE s_suppkey = " +
            std::to_string(suppkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 SafeInt32(ChunkValue(chunk, 1, row))};
          found = true;
        });
    return found;
  }
  bool LookupCustomer(int64_t custkey, TpchQ8CustomerInput &out) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT c_custkey, c_nationkey FROM customer WHERE c_custkey = " +
            std::to_string(custkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 SafeInt32(ChunkValue(chunk, 1, row))};
          found = true;
        });
    return found;
  }
  bool LookupOrder(int64_t orderkey, TpchQ8OrdersInput &out) override {
    bool found = false;
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_custkey, o_orderdate FROM orders "
        "WHERE o_orderkey = " + std::to_string(orderkey),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out = {SafeInt64(ChunkValue(chunk, 0, row)),
                 SafeInt64(ChunkValue(chunk, 1, row)),
                 DateDays(ChunkValue(chunk, 2, row))};
          found = true;
        });
    return found;
  }
  bool ScanLineitemsWhere(const std::string &from_where,
                          const LineitemConsumer &consumer) {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_partkey, l.l_orderkey, l.l_suppkey, l.l_extendedprice, "
        "l.l_discount " + from_where,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    DecimalRaw(ChunkValue(chunk, 3, row)),
                    DecimalRaw(ChunkValue(chunk, 4, row))});
        });
    return true;
  }
  bool ScanLineitemsOfPart(int64_t partkey,
                           const LineitemConsumer &consumer) override {
    return ScanLineitemsWhere(
        "FROM lineitem l WHERE l.l_partkey = " + std::to_string(partkey),
        consumer);
  }
  bool ScanLineitemsOfOrder(int64_t orderkey,
                            const LineitemConsumer &consumer) override {
    return ScanLineitemsWhere(
        "FROM lineitem l WHERE l.l_orderkey = " + std::to_string(orderkey),
        consumer);
  }
  bool ScanLineitemsOfOrders(const std::vector<int64_t> &orderkeys,
                             const LineitemConsumer &consumer) override {
    // One scan per chunk of keys instead of one query per order.
    constexpr std::size_t kChunk = 10000;
    for (std::size_t begin = 0; begin < orderkeys.size(); begin += kChunk) {
      const std::size_t end = std::min(orderkeys.size(), begin + kChunk);
      std::string list;
      for (std::size_t i = begin; i < end; ++i)
        list += (i > begin ? "," : "") + std::to_string(orderkeys[i]);
      ScanLineitemsWhere("FROM lineitem l WHERE l.l_orderkey IN (" + list + ")",
                         consumer);
    }
    return true;
  }
  bool ScanLineitemsOfSupplier(int64_t suppkey,
                               const LineitemConsumer &consumer) override {
    return ScanLineitemsWhere(
        "FROM lineitem l WHERE l.l_suppkey = " + std::to_string(suppkey),
        consumer);
  }
  bool ScanLineitemsOfCustomer(int64_t custkey,
                               const LineitemConsumer &consumer) override {
    return ScanLineitemsWhere(
        "FROM lineitem l JOIN orders o ON l.l_orderkey = o.o_orderkey "
        "WHERE o.o_custkey = " + std::to_string(custkey),
        consumer);
  }
  void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                 const std::vector<BuildColumn> &columns, std::size_t partitions,
                 const Sink &sink) override {
    ScanPartitioned(services, connection, table, columns, partitions, sink);
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ8Layout::kAccelerator,
    "accel_tpch_q8_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ8Config c{0, 1, 2, 3, 4, 5, 6};
      return MakeBinding<TpchQ8Scan, TpchQ8Module>(connection, CaptureTableList{{"part", c.part_table}, {"supplier", c.supplier_table}, {"customer", c.customer_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}, {"region", c.region_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
