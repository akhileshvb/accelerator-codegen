// GENERATED QUERY: TPC-H Q14 aux state on DuckDB.
// Registers the catalog entry (SQL, probes, benchmark arguments), the
// table function accel_tpch_q14_aux and the module's binding: its config, the
// tables engine capture watches, and the InitialScan over DuckDB SQL.
#include "tpch/q14/duckdb/aux/module.hpp"
#include "tpch/q14/cost_model.hpp"
#include "tpch/q14/layout.hpp"
#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/build_scan.hpp"
#include "catalog/query_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
namespace q14_catalog {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace catalog_values;

SharedProbe BuildTpchQ14Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Ratio is insensitive to +0.01 at SF1; use a large price bump so promo_revenue
  // moves beyond fingerprint precision.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  const int64_t price_raw = DecimalRaw(result.GetValue(2, row));
  constexpr int64_t kBumpRaw = 1000000000; // +10,000,000.00
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 10000000.0;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(5, row)),
                    Decimal(price_raw, 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Date(DateDays(result.GetValue(4, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(price_raw + kBumpRaw, 2);
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + 10000000.00"
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

const SharedQueryRegistrar kEntry{SharedQueryCatalogEntry{14,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ14Layout::kAccelerator,
     "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
     "(1 - l_discount) ELSE 0 END) / "
     "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
     "FROM lineitem, part "
     "WHERE l_partkey = p_partkey "
     "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + "
     "INTERVAL '1' month",
     "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
     "SELECT * FROM accel_tpch_q14(DATE '1995-06-01')",
     "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, "
     "l_partkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + "
     "INTERVAL '1' month ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-06-01"))})}},
     2,
     false,
     BuildTpchQ14Probe,
     core::kTpchQ14CostModel}};

} // namespace q14_catalog
} // namespace
} // namespace accelerator_codegen::aux_state::engine

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
using namespace accelerator_codegen::aux_state::query;
using namespace scan;

struct TpchQ14Scan : TpchQ14InitialScan {
  explicit TpchQ14Scan(::duckdb::Connection &connection) : connection(connection) {}
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, int64_t, bool)> &sink) override {
    ScanPartitioned(services, connection, "part",
                    {IntColumn("p_partkey"), TextColumn("p_type")}, partitions,
                    [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, row.Int(0), TpchQ14IsPromoType(row.TextView(1)));
                      }
                    });
  }
  static TpchQ14LineitemInput DecodeLineitem(const ::duckdb::DataChunk &chunk,
                                             ::duckdb::idx_t row) {
    return {SafeInt64(ChunkValue(chunk, 0, row)),
            DecimalRaw(ChunkValue(chunk, 1, row)), 2,
            DecimalRaw(ChunkValue(chunk, 2, row)), 2,
            DateDays(ChunkValue(chunk, 3, row))};
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const TpchQ14LineitemInput &)> &sink) override {
    ScanPartitioned(services, connection, "lineitem",
                    {IntColumn("l_partkey"), DecimalColumn("l_extendedprice", 2),
                     DecimalColumn("l_discount", 2), DateColumn("l_shipdate")},
                    partitions, [&](std::size_t partition, const ChunkBatch &batch) {
                      for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto row = batch.Row(i);
                        sink(partition, TpchQ14LineitemInput{row.Int(0), row.Decimal(1), 2,
                                                             row.Decimal(2), 2, row.Date(3)});
                      }
                    });
  }
  // Sargable shipdate ranges (zone-map friendly), one per month.
  static std::string MonthRanges(const std::set<int32_t> &months) {
    std::ostringstream out;
    bool first = true;
    for (const auto month : months) {
      auto year = month / 100;
      auto mon = month % 100;
      auto next_year = mon == 12 ? year + 1 : year;
      auto next_mon = mon == 12 ? 1 : mon + 1;
      char buf[128];
      std::snprintf(buf, sizeof(buf),
                    "(l_shipdate >= DATE '%04d-%02d-01' AND l_shipdate < DATE "
                    "'%04d-%02d-01')",
                    year, mon, next_year, next_mon);
      if (!first)
        out << " OR ";
      first = false;
      out << buf;
    }
    return out.str();
  }
  static constexpr const char *kRevenueRaw =
      "((l_extendedprice * 100)::BIGINT * (100 - (l_discount * 100)::BIGINT))";
  static constexpr const char *kMonthKey =
      "(year(l_shipdate) * 100 + month(l_shipdate))::BIGINT";
  void ScanLineitemsForMonths(
      const std::set<int32_t> &months,
      const std::function<void(const TpchQ14LineitemInput &)> &consumer)
      override {
    if (months.empty())
      return;
    const std::string sql =
        "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM "
        "lineitem WHERE " + MonthRanges(months);
    ScanQueryDecoded(connection, sql, DecodeLineitem,
                     [&](TpchQ14LineitemInput &&input) { consumer(input); });
  }
  bool ScanMonthAggregates(
      const std::set<int32_t> &months,
      const std::function<void(int32_t, int64_t, int64_t, int64_t)> &consumer)
      override {
    std::ostringstream sql;
    sql << "SELECT " << kMonthKey << ", SUM(" << kRevenueRaw
        << ")::BIGINT, SUM(CASE WHEN p_type LIKE 'PROMO%' THEN " << kRevenueRaw
        << " ELSE 0 END)::BIGINT, COUNT(*)::BIGINT FROM lineitem JOIN part ON "
           "l_partkey = p_partkey";
    if (!months.empty())
      sql << " WHERE " << MonthRanges(months);
    sql << " GROUP BY 1";
    ScanQueryStreaming(
        connection, sql.str(),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(static_cast<int32_t>(SafeInt64(ChunkValue(chunk, 0, row))),
                   SafeInt64(ChunkValue(chunk, 1, row)),
                   SafeInt64(ChunkValue(chunk, 2, row)),
                   SafeInt64(ChunkValue(chunk, 3, row)));
        });
    return true;
  }
  void ScanPartMonthRevenue(
      const std::vector<int64_t> &keys,
      const std::function<void(int64_t, int32_t, int64_t, int64_t)> &consumer)
      override {
    if (keys.empty())
      return;
    std::ostringstream sql;
    sql << "SELECT l_partkey, " << kMonthKey << ", SUM(" << kRevenueRaw
        << ")::BIGINT, COUNT(*)::BIGINT FROM lineitem WHERE l_partkey IN (";
    for (std::size_t i = 0; i < keys.size(); ++i)
      sql << (i ? ", " : "") << keys[i];
    sql << ") GROUP BY 1, 2";
    ScanQueryStreaming(
        connection, sql.str(),
        [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t row) {
          consumer(SafeInt64(ChunkValue(chunk, 0, row)),
                   static_cast<int32_t>(SafeInt64(ChunkValue(chunk, 1, row))),
                   SafeInt64(ChunkValue(chunk, 2, row)),
                   SafeInt64(ChunkValue(chunk, 3, row)));
        });
  }
  ::duckdb::Connection &connection;
};

const AuxQueryRegistrar kQuery{AuxQuery{
    core::TpchQ14Layout::kAccelerator,
    "accel_tpch_q14_aux",
    {::duckdb::LogicalType::DATE},
    [](::duckdb::Connection &connection) {
      const TpchQ14Config c{1, 2};
      return MakeBinding<TpchQ14Scan, TpchQ14Module>(connection, CaptureTableList{{"part", c.part_table}, {"lineitem", c.lineitem_table}}, c);
    }}};

} // namespace
} // namespace accelerator_codegen::aux_state::engine::duckdb
