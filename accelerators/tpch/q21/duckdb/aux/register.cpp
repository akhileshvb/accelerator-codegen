// TPC-H Q21 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q21_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q21/duckdb/aux/module.hpp"
#include "tpch/q21/cost_model.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;

constexpr core::TableId kSupplierTable = 1;
constexpr core::TableId kOrdersTable = 2;
constexpr core::TableId kLineitemTable = 3;
constexpr core::TableId kNationTable = 4;

namespace q21_catalog {

namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ21Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Clear "late receipt" on a Q21-qualifying line so numwait drops.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int32_t commit_days = DateDays(result.GetValue(3, row));
  const int32_t receipt_days = DateDays(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = kLineitemTable;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(receipt_days);
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(2, row)),
                    Date(commit_days), Date(receipt_days)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Date(commit_days);
  const auto commit_date =
      ::duckdb::Date::ToString(::duckdb::date_t{commit_days});
  const auto receipt_date =
      ::duckdb::Date::ToString(::duckdb::date_t{receipt_days});
  probe.update_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + commit_date +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + receipt_date +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
  probe.toggles = true;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{21,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ21Layout::kAccelerator,
     "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
     "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
     "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
     "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
     "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
     "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
     "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
     "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100",
     "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
     "SELECT * FROM accel_tpch_q21('SAUDI ARABIA')",
     "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
     "SELECT l1.l_orderkey, l1.l_linenumber, l1.l_suppkey, l1.l_commitdate, "
     "l1.l_receiptdate "
     "FROM supplier, lineitem l1, orders, nation "
     "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
     "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
     "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
     "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
     "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
     "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
     "n_name = 'SAUDI ARABIA' "
     "ORDER BY l1.l_orderkey, l1.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"SAUDI ARABIA"})}},
     kLineitemTable,
     false,
     BuildTpchQ21Probe,
     core::kTpchQ21CostModel,
     "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation "
     "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F' "
     "AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ( "
     "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey) "
     "AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey "
     "AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate) "
     "AND s_nationkey = n_nationkey AND n_name = $1 "
     "GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100"}};

} // namespace q21_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ21Scan : TpchQ21InitialScan {
  // Order keys per point-lookup query.
  static constexpr std::size_t kLookupBatch = 4096;
  explicit TpchQ21Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ21SupplierInput &)> &sink) override {
    ScanPartitioned(services, connection, "supplier",
                    {IntColumn("s_suppkey"), TextColumn("s_name"), IntColumn("s_nationkey")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        const auto nationkey = row.Int(2);
                        if (nationkey != static_cast<int32_t>(nationkey))
                          throw std::runtime_error("TPC-H Q21: s_nationkey exceeds 32 bits");
                        sink(partition, TpchQ21SupplierInput{row.Int(0), row.Text(1),
                                                             static_cast<int32_t>(nationkey)});
                      }
                    });
  }
  bool LookupOrderSummary(int64_t orderkey, OrderLookup &out) override {
    out = OrderLookup{};
    const std::string key = std::to_string(orderkey);
    ScanQueryStreaming(
        connection,
        "SELECT o_orderstatus FROM orders WHERE o_orderkey = " + key,
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out.found = true;
          out.status = FirstChar(ChunkValue(chunk, 0, row));
        });
    if (!out.found)
      return true;
    ScanQueryStreaming(
        connection,
        "SELECT l_suppkey, COUNT(*), "
        "SUM(CASE WHEN l_receiptdate > l_commitdate THEN 1 ELSE 0 END) "
        "FROM lineitem WHERE l_orderkey = " + key + " GROUP BY l_suppkey",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          out.suppliers.push_back({SafeInt64(ChunkValue(chunk, 0, row)),
                                   SafeInt64(ChunkValue(chunk, 1, row)),
                                   SafeInt64(ChunkValue(chunk, 2, row))});
        });
    return true;
  }
  bool LookupOrderSummaries(const std::vector<int64_t> &orderkeys,
                            std::vector<OrderLookup> &out) override {
    out.assign(orderkeys.size(), OrderLookup{});
    std::unordered_map<int64_t, std::size_t> position;
    position.reserve(orderkeys.size());
    for (std::size_t i = 0; i < orderkeys.size(); ++i)
      position.emplace(orderkeys[i], i);
    for (std::size_t first = 0; first < orderkeys.size(); first += kLookupBatch) {
      std::string keys;
      const auto last = std::min(orderkeys.size(), first + kLookupBatch);
      for (std::size_t i = first; i < last; ++i)
        keys += (i == first ? "" : ",") + std::to_string(orderkeys[i]);
      ScanQueryStreaming(
          connection,
          "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderkey IN (" + keys + ")",
          [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
            auto &lookup = out[position.at(SafeInt64(ChunkValue(chunk, 0, row)))];
            lookup.found = true;
            lookup.status = FirstChar(ChunkValue(chunk, 1, row));
          });
      ScanQueryStreaming(
          connection,
          "SELECT l_orderkey, l_suppkey, COUNT(*), "
          "SUM(CASE WHEN l_receiptdate > l_commitdate THEN 1 ELSE 0 END) "
          "FROM lineitem WHERE l_orderkey IN (" + keys + ") GROUP BY l_orderkey, l_suppkey",
          [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
            auto &lookup = out[position.at(SafeInt64(ChunkValue(chunk, 0, row)))];
            if (lookup.found)
              lookup.suppliers.push_back({SafeInt64(ChunkValue(chunk, 1, row)),
                                          SafeInt64(ChunkValue(chunk, 2, row)),
                                          SafeInt64(ChunkValue(chunk, 3, row))});
          });
    }
    return true;
  }
  void ScanNations(
      const std::function<void(const TpchQ21NationInput &)> &consumer) override {
    ScanQuery(connection, "SELECT n_nationkey, n_name FROM nation",
              [&](auto &result, ::duckdb::idx_t row) {
                consumer({SafeInt32(result.GetValue(0, row)),
                          result.GetValue(1, row).ToString()});
              });
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ21OrderInput &)> &sink) override {
    ScanPartitioned(services, connection, "orders",
                    {IntColumn("o_orderkey"), TextColumn("o_orderstatus")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ21OrderInput{row.Int(0), row.Char(1)});
                      }
                    });
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ21LineitemInput &)> &sink) override {
    ScanPartitioned(services, connection, "lineitem",
                    {IntColumn("l_orderkey"), IntColumn("l_suppkey"), DateColumn("l_commitdate"),
                     DateColumn("l_receiptdate")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ21LineitemInput{row.Int(0), row.Int(1), row.Date(2),
                                                             row.Date(3)});
                      }
                    });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ21Layout::kAccelerator,
    "accel_tpch_q21_aux",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ21Config c{kSupplierTable, kOrdersTable, kLineitemTable, kNationTable};
      return MakeBinding<TpchQ21Scan, TpchQ21Module>(connection, CaptureTableList{{"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
