// GENERATED QUERY: TPC-H Q17 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q17_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q17/duckdb/aux/module.hpp"
#include "tpch/q17/cost_model.hpp"
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
namespace q17_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ17Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Bump extendedprice on a row already in the Q17 brand/container filter so
  // avg_yearly moves (tiny quantity bumps can be invisible after /7.0).
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t price_raw = DecimalRaw(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(price_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(2, row)), Decimal(quantity_raw, 2),
                    Decimal(price_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(price_raw + 100, 2); // +1.00
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + 1.00 WHERE l_orderkey = " +
      orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql =
      "UPDATE lineitem SET l_extendedprice = " +
      std::to_string(static_cast<double>(price_raw) / 100.0) +
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{17,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ17Layout::kAccelerator,
     // DuckDB TPC-H uses abbreviated containers (LG CASE), not official
     // syllable pairs (LARGE BRUSHED). Defaults must match the loaded DB.
     "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
     "AND l_quantity < avg_quantity",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')",
     "SELECT * FROM accel_tpch_q17('Brand#34', 'LG CASE')",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_quantity, l_extendedprice "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#34' AND p_container = 'LG CASE' "
     "AND l_quantity < avg_quantity "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#34"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"LG CASE"})}},
     2,
     false,
     BuildTpchQ17Probe,
     core::kTpchQ17CostModel}};

} // namespace q17_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ17Scan : TpchQ17InitialScan {
  explicit TpchQ17Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const std::function<void(const TpchQ17PartInput &)> &consumer) override {
    ScanQueryStreaming(
        connection, "SELECT p_partkey, p_brand, p_container FROM part",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    ChunkValue(chunk, 1, row).ToString(),
                    ChunkValue(chunk, 2, row).ToString()});
        });
  }
  void ScanLineitems(
      const std::function<void(const TpchQ17LineitemInput &)> &consumer) override {
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem" +
            OptionalBenchScanLimitSuffix("ACCEL_Q17_BENCH_SCAN_LIMIT"),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    DecimalRaw(ChunkValue(chunk, 1, row)),
                    DecimalRaw(ChunkValue(chunk, 2, row))});
        });
  }
  bool ScanPartQuantityAggregates(
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    // Bench scan limits apply to the row path only.
    if (std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT")) {
      return false;
    }
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, "
        "CAST(round(l_quantity * 100) AS BIGINT), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "COUNT(*)::BIGINT "
        "FROM lineitem "
        "GROUP BY 1, 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    if (std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT")) {
      return false;
    }
    ScanQueryStreaming(
        connection,
        "SELECT " + std::to_string(partkey) +
            "::BIGINT, "
            "CAST(round(l_quantity * 100) AS BIGINT), "
            "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
            "COUNT(*)::BIGINT "
            "FROM lineitem WHERE l_partkey = " +
            std::to_string(partkey) + " GROUP BY 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  bool ScanPartQuantityAggregatesForParts(
      const std::unordered_set<int64_t> &partkeys,
      const std::function<void(const TpchQ17PartQuantityAggregate &)> &consumer)
      override {
    if (partkeys.empty() || std::getenv("ACCEL_Q17_BENCH_SCAN_LIMIT"))
      return false;
    if (partkeys.size() == 1)
      return ScanPartQuantityAggregatesForPart(*partkeys.begin(), consumer);
    ScanQueryStreaming(
        connection,
        "SELECT l_partkey, "
        "CAST(round(l_quantity * 100) AS BIGINT), "
        "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
        "COUNT(*)::BIGINT "
        "FROM lineitem WHERE " +
            SqlInt64InPredicate("l_partkey", partkeys) +
            " GROUP BY 1, 2",
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer({SafeInt64(ChunkValue(chunk, 0, row)),
                    SafeInt64(ChunkValue(chunk, 1, row)),
                    SafeInt64(ChunkValue(chunk, 2, row)),
                    SafeInt64(ChunkValue(chunk, 3, row))});
        });
    return true;
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    query::TpchQ17Layout::kAccelerator,
    "accel_tpch_q17_aux",
    {::duckdb::LogicalType::VARCHAR, ::duckdb::LogicalType::VARCHAR},
    [](::duckdb::Connection &connection) {
      const TpchQ17Config c{1, 2};
      return MakeBinding<TpchQ17Scan, TpchQ17Module>(connection, CaptureTableList{{"part", c.part_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
