// GENERATED QUERY: TPC-H Q10 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q10_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q10/duckdb/aux/module.hpp"
#include "tpch/q10/cost_model.hpp"
#include "tpch/q10/layout.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

constexpr accelerator_codegen::aux_state::query::TpchQ10Config kTables{1, 2, 3, 4};

namespace q10_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ10Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = kTables.lineitem_table;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Text(result.GetValue(4, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{10,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ10Layout::kAccelerator,
     "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
     "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
     "FROM customer JOIN orders ON c_custkey = o_custkey "
     "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
     "c_nationkey = n_nationkey "
     "WHERE o_orderdate >= DATE '1993-06-01' "
     "AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH AND "
     "l_returnflag = 'R' "
     "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
     "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     // Returned lineitems that feed the TOP-20 customers so revenue moves.
     "SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount, "
     "l.l_returnflag FROM lineitem l "
     "JOIN orders o ON l.l_orderkey = o.o_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "WHERE l.l_returnflag = 'R' "
     "AND o.o_orderdate >= DATE '1993-06-01' "
     "AND o.o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
     "AND c.c_custkey IN ("
     "SELECT c_custkey FROM ("
     "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM customer JOIN orders ON c_custkey = o_custkey "
     "JOIN lineitem ON l_orderkey = o_orderkey "
     "WHERE o_orderdate >= DATE '1993-06-01' "
     "AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
     "AND l_returnflag = 'R' "
     "GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20))"
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1993-06-01"))})}},
     4,
     false,
     BuildTpchQ10Probe,
     core::kTpchQ10CostModel,
     "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
     "c_acctbal, n_name, c_address, c_phone, c_comment "
     "FROM customer JOIN orders ON c_custkey = o_custkey "
     "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON c_nationkey = n_nationkey "
     "WHERE o_orderdate >= $1 AND o_orderdate < $1 + INTERVAL '3' MONTH AND l_returnflag = 'R' "
     "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment "
     "ORDER BY revenue DESC, c_custkey LIMIT 20"}};

} // namespace q10_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ10Scan : TpchQ10InitialScan {
  explicit TpchQ10Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ10CustomerInput &)> &consumer) override {
    ScanQueryDecoded(
        connection,
        "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
        "c_phone, c_comment FROM customer",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ10CustomerInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                      ChunkValue(chunk, 1, row).ToString(),
                                      DecimalRaw(ChunkValue(chunk, 2, row)),
                                      SafeInt32(ChunkValue(chunk, 3, row)),
                                      ChunkValue(chunk, 4, row).ToString(),
                                      ChunkValue(chunk, 5, row).ToString(),
                                      ChunkValue(chunk, 6, row).ToString()};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanNations(
      const std::function<void(const TpchQ10NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const std::function<void(const TpchQ10OrderInput &)> &consumer) override {
    ScanQueryDecoded(
        connection, "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ10OrderInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                   SafeInt64(ChunkValue(chunk, 1, row)),
                                   DateDays(ChunkValue(chunk, 2, row))};
        },
        [&](auto &&input) { consumer(input); });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ10LineitemInput &)> &consumer) override {
    // Fallback path only: filter returned rows in SQL when the C++ join is used.
    ScanQueryDecoded(
        connection,
        "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
        "FROM lineitem WHERE l_returnflag = 'R'",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ10LineitemInput{SafeInt64(ChunkValue(chunk, 0, row)),
                                      DecimalRaw(ChunkValue(chunk, 1, row)), 2,
                                      DecimalRaw(ChunkValue(chunk, 2, row)), 2, true};
        },
        [&](auto &&input) { consumer(input); });
  }
  bool ScanReturnedCustomerRevenue(
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer)
      override {
    // Push returned-lineitem ⨝ orders aggregation into DuckDB. Revenue raw
    // matches TpchQ10RevenueRaw: round(extended * (1-discount) * 10000) per
    // line, then sum. Stream chunks — do not materialize the full GROUP BY.
    ScanQueryDecoded(
        connection,
        "SELECT CAST(year(o_orderdate) * 100 + month(o_orderdate) AS INTEGER), "
        "o_custkey, "
        "sum(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)) "
        "FROM lineitem "
        "JOIN orders ON l_orderkey = o_orderkey "
        "WHERE l_returnflag = 'R' "
        "GROUP BY 1, 2",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ10ReturnedRevenueInput{SafeInt32(ChunkValue(chunk, 0, row)),
                                             SafeInt64(ChunkValue(chunk, 1, row)),
                                             SafeInt64(ChunkValue(chunk, 2, row))};
        },
        [&](auto &&input) { consumer(input); });
    return true;
  }
  bool ScanReturnedCustomerRevenueForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ10ReturnedRevenueInput &)> &consumer)
      override {
    if (months.empty())
      return true;
    ScanQueryDecoded(
        connection,
        "SELECT CAST(year(o_orderdate) * 100 + month(o_orderdate) AS INTEGER), "
        "o_custkey, "
        "sum(CAST(round(l_extendedprice * (1 - l_discount) * 10000) AS BIGINT)) "
        "FROM lineitem "
        "JOIN orders ON l_orderkey = o_orderkey "
        "WHERE l_returnflag = 'R' AND " +
            SqlIntInPredicate(
                "(year(o_orderdate) * 100 + month(o_orderdate))", months) +
            " GROUP BY 1, 2",
        [](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          return TpchQ10ReturnedRevenueInput{SafeInt32(ChunkValue(chunk, 0, row)),
                                             SafeInt64(ChunkValue(chunk, 1, row)),
                                             SafeInt64(ChunkValue(chunk, 2, row))};
        },
        [&](auto &&input) { consumer(input); });
    return true;
  }
  bool LookupOrders(
      const std::vector<int64_t> &orderkeys,
      std::unordered_map<int64_t, TpchQ10Order> &out) override {
    out.clear();
    if (orderkeys.empty()) {
      return true;
    }
    std::ostringstream sql;
    sql << "SELECT o_orderkey, o_custkey, o_orderdate FROM orders "
           "WHERE o_orderkey IN (";
    for (std::size_t i = 0; i < orderkeys.size(); ++i) {
      if (i > 0) {
        sql << ',';
      }
      sql << orderkeys[i];
    }
    sql << ')';
    ScanQueryStreaming(connection, sql.str(),
                       [&](const ::duckdb::DataChunk &chunk,
                           ::duckdb::idx_t row) {
                         out.emplace(SafeInt64(ChunkValue(chunk, 0, row)),
                                     TpchQ10Order{
                                         SafeInt64(ChunkValue(chunk, 1, row)),
                                         DateDays(ChunkValue(chunk, 2, row))});
                       });
    return true;
  }
  bool OrdersWithReturnedLineitems(const std::vector<int64_t> &orderkeys,
                                   std::set<int64_t> &out) override {
    out.clear();
    if (orderkeys.empty())
      return true;
    std::ostringstream sql;
    sql << "SELECT DISTINCT l_orderkey FROM lineitem "
           "WHERE l_returnflag = 'R' AND l_orderkey IN (";
    for (std::size_t i = 0; i < orderkeys.size(); ++i) {
      if (i > 0)
        sql << ',';
      sql << orderkeys[i];
    }
    sql << ')';
    ScanQueryStreaming(connection, sql.str(),
                       [&](const ::duckdb::DataChunk &chunk,
                           ::duckdb::idx_t row) {
                         out.insert(SafeInt64(ChunkValue(chunk, 0, row)));
                       });
    return true;
  }
  void ScanTable(const core::AuxEngineServices &services, const std::string &table,
                 const std::vector<BuildColumn> &columns, std::size_t partitions,
                 const Sink &sink) override {
    ScanPartitioned(services, connection, table, columns, partitions, sink);
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ10Layout::kAccelerator,
    "accel_tpch_q10_aux",
    {::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      return MakeBinding<TpchQ10Scan, TpchQ10Module>(
          connection,
          CaptureTableList{{"customer", kTables.customer_table},
                           {"nation", kTables.nation_table},
                           {"orders", kTables.orders_table},
                           {"lineitem", kTables.lineitem_table}},
          kTables);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
