// MySQL plugin runtime for TPC-H Q14: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include <stdexcept>

#include "accelerator_codegen/stateful/core/decimal_text.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q14/mysql/aux/module.hpp"
#include "tpch/q14/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ14Parts(
    const std::function<void(const query::TpchQ14PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

// lineitem JOIN part GROUP BY shipdate in the engine: one row per day with the
// exact revenue sums (decimal text, scale 4). `LIKE` follows the column's
// collation, as in the reference query.
void ScanTpchQ14DayAggregates(
    const std::function<void(const query::TpchQ14DayAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuples(
      "SELECT l_shipdate, sum(l_extendedprice * (1 - l_discount)), "
      "sum(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
      "(1 - l_discount) ELSE 0 END), count(*) "
      "FROM lineitem, part WHERE l_partkey = p_partkey GROUP BY l_shipdate",
      [&](const std::vector<std::string> &cols) {
        query::TpchQ14DayAggregate agg;
        agg.shipdate_days = TypedDate(cols, 0);
        if (!dec::Parse(TypedText(cols, 1), 4, agg.total_revenue_raw) ||
            !dec::Parse(TypedText(cols, 2), 4, agg.promo_revenue_raw))
          throw std::runtime_error("TPC-H Q14: unparsable decimal");
        agg.count = TypedInt(cols, 3);
        consumer(agg);
      });
}

// Point re-aggregation of the given parts' lineitems (the schema has no
// l_partkey index: this is a table scan on MySQL).
void ScanTpchQ14PartDayRevenue(
    const std::vector<int64_t> &partkeys,
    const std::function<void(const query::TpchQ14PartDayRevenue &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  if (partkeys.empty())
    return;
  std::string keys;
  for (const auto key : partkeys)
    keys += (keys.empty() ? "" : ",") + std::to_string(key);
  ScanTypedTuples(
      "SELECT l_partkey, l_shipdate, sum(l_extendedprice * (1 - l_discount)), "
      "count(*) FROM lineitem WHERE l_partkey IN (" + keys +
          ") GROUP BY l_partkey, l_shipdate",
      [&](const std::vector<std::string> &cols) {
        query::TpchQ14PartDayRevenue row;
        row.partkey = TypedInt(cols, 0);
        row.shipdate_days = TypedDate(cols, 1);
        if (!dec::Parse(TypedText(cols, 2), 4, row.revenue_raw))
          throw std::runtime_error("TPC-H Q14: unparsable decimal");
        row.count = TypedInt(cols, 3);
        consumer(row);
      });
}

} // namespace scans

class TpchQ14Scan final : public query::TpchQ14InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ14PartInput &)> &consumer)
      override {
    scans::ScanTpchQ14Parts(consumer);
  }
  void ScanDayAggregates(
      const std::function<void(const query::TpchQ14DayAggregate &)> &consumer)
      override {
    scans::ScanTpchQ14DayAggregates(consumer);
  }
  void ScanPartDayRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const query::TpchQ14PartDayRevenue &)> &consumer)
      override {
    scans::ScanTpchQ14PartDayRevenue(partkeys, consumer);
  }
};

Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan> &
Q14Runtime() {
  static Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q14_COST_MODEL",
              "config/q14_aux_cost_model.json",
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{mysql_tbl::kPart, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q14Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ14Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q14_aux", "d", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
