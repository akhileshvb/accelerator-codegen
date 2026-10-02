// TPC-H Q21 served by the Verus-verified kernel on DuckDB: the accelerator
// "tpch_q21_verus" (table function accel_tpch_q21_verus), a variant of the
// Q21 catalog query (same SQL, probes and arguments).
#include "tpch/q21/verus/module.hpp"
#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include "catalog/query_catalog.hpp"

#include <functional>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).  All run on the runtime's
// connection, so inside a write transaction they see its own writes.
struct TpchQ21VerusDuckScan : TpchQ21VerusScan {
  explicit TpchQ21VerusDuckScan(::duckdb::Connection &connection)
      : connection(connection) {}
  static q21v_li_row Row(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
    return q21v_li_row{SafeInt64(ChunkValue(chunk, 0, row)),
                       SafeInt64(ChunkValue(chunk, 1, row)),
                       DateDays(ChunkValue(chunk, 2, row)),
                       DateDays(ChunkValue(chunk, 3, row))};
  }
  void ScanSuppliers(const std::function<void(int64_t, const std::string &, int32_t)>
                         &consumer) override {
    ScanQueryStreaming(connection, "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                                  ChunkValue(chunk, 1, row).ToString(),
                                  SafeInt32(ChunkValue(chunk, 2, row)));
                       });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanQueryStreaming(connection, "SELECT n_nationkey, n_name FROM nation",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt32(ChunkValue(chunk, 0, row)),
                                  ChunkValue(chunk, 1, row).ToString());
                       });
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanQueryStreaming(connection,
                       "SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(SafeInt64(ChunkValue(chunk, 0, row)));
                       });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
        "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(Row(chunk, row));
        });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    const std::string sql =
        "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem WHERE " +
        SqlInt64InPredicate("l_orderkey", {keys.begin(), keys.end()});
    ScanQueryStreaming(connection, sql,
                       [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
                         consumer(Row(chunk, row));
                       });
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    auto stmt = ScanConnection(connection).Prepare(
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = $1 GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100");
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "Q21 prepare failed");
    // Execute(args...) streams by default; the cast below needs a materialized result.
    ::duckdb::vector<::duckdb::Value> params{::duckdb::Value(nation)};
    auto result = stmt->Execute(params, /*allow_stream_result=*/false);
    Require(result && !result->HasError(), result ? result->GetError() : "Q21 failed");
    auto &materialized = dynamic_cast<::duckdb::MaterializedQueryResult &>(*result);
    for (::duckdb::idx_t row = 0; row < materialized.RowCount(); ++row)
      consumer(materialized.GetValue(0, row).ToString(),
               SafeInt64(materialized.GetValue(1, row)));
  }
  ::duckdb::Connection &connection;
};

const SharedQueryVariantRegistrar kVariant{TpchQ21VerusLayout::kAccelerator, SharedWorkloadKind::Tpch, 21,
                                           "SELECT * FROM accel_tpch_q21_verus('SAUDI ARABIA')"};

const AuxQueryRegistrar kQuery{AuxQuery{
    TpchQ21VerusLayout::kAccelerator,
    "accel_tpch_q21_verus",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ21VerusConfig c{1, 2, 3, 4};
      return MakeBinding<TpchQ21VerusDuckScan, TpchQ21VerusModule>(
          connection,
          CaptureTableList{{"supplier", c.supplier_table}, {"orders", c.orders_table},
                           {"lineitem", c.lineitem_table}, {"nation", c.nation_table}},
          c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
