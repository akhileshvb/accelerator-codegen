// GENERATED QUERY: TPC-H Q13 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q13_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q13/duckdb/aux/module.hpp"
#include "tpch/q13/layout.hpp"
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
namespace q13_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ13Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto comment = result.GetValue(2, row).ToString();
  SharedProbe probe;
  probe.table = 1;  // TpchQ13Config{0,1}.orders_table
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(1, row)), Text(result.GetValue(2, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::String, comment + " unusual accounts");
  probe.update_sql =
      "UPDATE orders SET o_comment = o_comment || ' unusual accounts' "
      "WHERE o_orderkey = " + orderkey;
  probe.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(comment) +
                      " WHERE o_orderkey = " + orderkey;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{13,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ13Layout::kAccelerator,
     "SELECT c_count, COUNT(*) AS custdist FROM ("
     "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
     "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
     "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
     "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT o_orderkey, o_custkey, o_comment FROM orders "
     "WHERE o_comment NOT LIKE '%unusual%accounts%' "
     "ORDER BY o_orderkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String,
                          std::string{"%unusual%accounts%"})}},
     1,
     false,
     BuildTpchQ13Probe}};

} // namespace q13_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ13Scan : TpchQ13InitialScan {
  explicit TpchQ13Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanCustomers(
      const std::function<void(const TpchQ13CustomerInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT c_custkey FROM customer",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row))});
        });
  }
  void ScanOrders(
      const std::function<void(const TpchQ13OrderInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT o_custkey, o_comment FROM orders "
        "WHERE o_custkey IS NOT NULL AND o_comment IS NOT NULL",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString()});
        });
  }
  bool ShardSizes(std::vector<TpchQ13ShardSize> &out) override {
    std::unordered_map<uint32_t, TpchQ13ShardSize> sizes;
    ScanQuery(connection,
              "SELECT o_custkey % 16384, count(*), sum(strlen(o_comment)) "
              "FROM orders WHERE o_custkey >= 0 AND o_custkey < 68719476736 "
              "AND o_comment IS NOT NULL GROUP BY 1",
              [&](::duckdb::MaterializedQueryResult &r, ::duckdb::idx_t row) {
                auto &s = sizes[static_cast<uint32_t>(SafeInt64(r.GetValue(0, row)))];
                s.orders = static_cast<uint64_t>(SafeInt64(r.GetValue(1, row)));
                s.comment_bytes = static_cast<uint64_t>(SafeInt64(r.GetValue(2, row)));
              });
    ScanQuery(connection,
              "SELECT c_custkey % 16384, max(c_custkey // 16384) + 1 FROM customer "
              "WHERE c_custkey >= 0 AND c_custkey < 68719476736 GROUP BY 1",
              [&](::duckdb::MaterializedQueryResult &r, ::duckdb::idx_t row) {
                auto &s = sizes[static_cast<uint32_t>(SafeInt64(r.GetValue(0, row)))];
                s.cust_slots = static_cast<uint64_t>(SafeInt64(r.GetValue(1, row)));
              });
    for (auto &[shard, size] : sizes) {
      size.shard = shard;
      out.push_back(size);
    }
    return true;
  }
  // The initial load: column vectors read in place (no Value or std::string
  // per row).
  void ScanOrdersView(const std::function<void(int64_t, const char *, std::size_t)>
                          &consumer) override {
    auto scan_lock = LockScan(connection);
    auto &conn = ScanConnection(connection);
    auto result = conn.SendQuery(
        "SELECT o_custkey, o_comment FROM orders "
        "WHERE o_custkey IS NOT NULL AND o_comment IS NOT NULL");
    Require(result != nullptr && !result->HasError(),
            result ? result->GetError() : "DuckDB scan returned null");
    for (;;) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0)
        break;
      const auto n = chunk->size();
      ::duckdb::UnifiedVectorFormat keys, comments;
      chunk->data[0].ToUnifiedFormat(n, keys);
      chunk->data[1].ToUnifiedFormat(n, comments);
      const auto *cd = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(comments);
      const auto kt = chunk->data[0].GetType().id();
      for (::duckdb::idx_t i = 0; i < n; ++i) {
        const auto ki = keys.sel->get_index(i), ci = comments.sel->get_index(i);
        const int64_t key =
            kt == ::duckdb::LogicalTypeId::BIGINT
                ? ::duckdb::UnifiedVectorFormat::GetData<int64_t>(keys)[ki]
            : kt == ::duckdb::LogicalTypeId::INTEGER
                ? int64_t{::duckdb::UnifiedVectorFormat::GetData<int32_t>(keys)[ki]}
                : SafeInt64(chunk->GetValue(0, i));
        consumer(key, cd[ci].GetData(), cd[ci].GetSize());
      }
    }
    Require(!result->HasError(), result->GetError());
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ13Layout::kAccelerator,
    "accel_tpch_q13_aux",
    {::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ13Config c{0, 1};
      return MakeBinding<TpchQ13Scan, TpchQ13Module>(connection, CaptureTableList{{"customer", c.customer_table}, {"orders", c.orders_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
