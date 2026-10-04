// GENERATED QUERY: TPC-H Q21 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q21_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q21/duckdb/aux/module.hpp"
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
namespace q21_catalog {

namespace core = accelerator_codegen::aux_state::core;
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
  probe.table = 3;
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
     "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name",
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
     3,
     false,
     BuildTpchQ21Probe}};

} // namespace q21_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

// Typed streaming scan: reads DuckDB vectors directly (no boxed Values).
template <typename T> struct TypedCol {
  ::duckdb::UnifiedVectorFormat fmt;
  const T *data = nullptr;
  void Init(::duckdb::DataChunk &chunk, ::duckdb::idx_t col) {
    chunk.data[col].ToUnifiedFormat(chunk.size(), fmt);
    data = ::duckdb::UnifiedVectorFormat::GetData<T>(fmt);
  }
  bool Valid(::duckdb::idx_t row) const {
    return fmt.validity.RowIsValid(fmt.sel->get_index(row));
  }
  const T &operator()(::duckdb::idx_t row) const {
    return data[fmt.sel->get_index(row)];
  }
};

template <typename Callback>
void ScanTypedChunks(::duckdb::Connection &connection_p, const std::string &sql,
                     Callback &&callback) {
  auto &connection = ScanConnection(connection_p);
  auto result = connection.SendQuery(sql);
  if (!result || result->HasError())
    throw std::runtime_error(result ? result->GetError()
                                    : "DuckDB scan returned null");
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0)
      break;
    callback(*chunk);
  }
  if (result->HasError())
    throw std::runtime_error(result->GetError());
}

struct TpchQ21Scan : TpchQ21InitialScan {
  explicit TpchQ21Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanSuppliers(
      const std::function<void(const TpchQ21SupplierInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    SafeInt32(ChunkValue(chunk, 2, row))});
        });
  }
  bool ScanContributingOrderSuppliers(
      const std::function<void(int64_t, int64_t, int64_t, int64_t)> &consumer)
      override {
    // Fulfilled orders with >= 2 suppliers and exactly one late supplier: the
    // only orders that can contribute. Per-(order, supplier) counts are
    // computed in SQL; no per-lineitem C++ work.
    ScanTypedChunks(
        connection,
        "SELECT CAST(l.l_orderkey AS BIGINT), CAST(l.l_suppkey AS BIGINT), "
        "COUNT(*), CAST(SUM(CASE WHEN l.l_receiptdate > l.l_commitdate THEN 1 ELSE 0 END) AS BIGINT) "
        "FROM lineitem l JOIN ("
        "SELECT l2.l_orderkey AS k FROM lineitem l2 JOIN orders o2 "
        "ON l2.l_orderkey = o2.o_orderkey AND o2.o_orderstatus = 'F' "
        "GROUP BY l2.l_orderkey "
        "HAVING COUNT(DISTINCT l2.l_suppkey) > 1 AND "
        "COUNT(DISTINCT CASE WHEN l2.l_receiptdate > l2.l_commitdate "
        "THEN l2.l_suppkey END) = 1) c ON l.l_orderkey = c.k "
        "GROUP BY l.l_orderkey, l.l_suppkey" +
            OptionalBenchScanLimitSuffix("ACCEL_Q21_BENCH_SCAN_LIMIT"),
        [&](::duckdb::DataChunk &chunk) {
          TypedCol<int64_t> ok, sk, tot, late;
          ok.Init(chunk, 0);
          sk.Init(chunk, 1);
          tot.Init(chunk, 2);
          late.Init(chunk, 3);
          for (::duckdb::idx_t i = 0; i < chunk.size(); ++i) {
            if (!ok.Valid(i) || !sk.Valid(i))
              continue;
            consumer(ok(i), sk(i), tot(i), late.Valid(i) ? late(i) : 0);
          }
        });
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
      const std::function<void(const TpchQ21OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    FirstChar(ChunkValue(chunk, 1, row))});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ21LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l "
        "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
        "o.o_orderstatus = 'F'" +
            OptionalBenchScanLimitSuffix("ACCEL_Q21_BENCH_SCAN_LIMIT"),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    DateDays(ChunkValue(chunk, 2, row)),
                    DateDays(ChunkValue(chunk, 3, row))});
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ21Layout::kAccelerator,
    "accel_tpch_q21_aux",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ21Config c{1, 2, 3, 4};
      return MakeBinding<TpchQ21Scan, TpchQ21Module>(connection, CaptureTableList{{"supplier", c.supplier_table}, {"orders", c.orders_table}, {"lineitem", c.lineitem_table}, {"nation", c.nation_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
