// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q14 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q14/postgres/aux/module.hpp"
#include "tpch/q14/cost_model.hpp"
#include "tpch/q14/layout.hpp"
#include <stdexcept>


#include "prims/decimal_text.hpp"
#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ14Parts(
    const std::function<void(const query::TpchQ14PartInput &)> &consumer) {
  ScanColumns("part",
              {IntColumn("p_partkey"), TextColumn("p_type")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Text(1)});
              });
}

// lineitem JOIN part GROUP BY shipdate in the engine: one row per day with the
// exact revenue sums (text, scale 4).
void ScanTpchQ14DayAggregates(
    const std::function<void(const query::TpchQ14DayAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuplesParallel(
      "SELECT l_shipdate, sum(l_extendedprice * (1 - l_discount)), "
      "sum(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
      "(1 - l_discount) ELSE 0 END), count(*) "
      "FROM lineitem, part WHERE l_partkey = p_partkey GROUP BY l_shipdate",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ14DayAggregate agg;
        agg.shipdate_days = TypedDate(tuple, tuple_desc, 0);
        if (!dec::Parse(TypedText(tuple, tuple_desc, 1), 4,
                        agg.total_revenue_raw) ||
            !dec::Parse(TypedText(tuple, tuple_desc, 2), 4,
                        agg.promo_revenue_raw))
          throw std::runtime_error("TPC-H Q14: unparsable numeric");
        agg.count = TypedInt(tuple, tuple_desc, 3);
        consumer(agg);
      });
}

// Point re-aggregation of the given parts' lineitems (l_partkey is indexed).
void ScanTpchQ14PartDayRevenue(
    const std::vector<int64_t> &partkeys,
    const std::function<void(const query::TpchQ14PartDayRevenue &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuplesAfterOwnWrites(
      "SELECT l_partkey, l_shipdate, sum(l_extendedprice * (1 - l_discount)), "
      "count(*) FROM lineitem WHERE " +
          KeyListPredicate("l_partkey", partkeys) +
          " GROUP BY l_partkey, l_shipdate",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ14PartDayRevenue row;
        row.partkey = TypedInt(tuple, tuple_desc, 0);
        row.shipdate_days = TypedDate(tuple, tuple_desc, 1);
        if (!dec::Parse(TypedText(tuple, tuple_desc, 2), 4, row.revenue_raw))
          throw std::runtime_error("TPC-H Q14: unparsable numeric");
        row.count = TypedInt(tuple, tuple_desc, 3);
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
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q14_COST_MODEL",
              core::kTpchQ14CostModel,
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ14(FunctionCallInfo fcinfo) {
  // A NULL date makes the predicate NULL: no rows qualify.
  detail::Q14Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 14, kPostgresVariantAux, &ExecutePostgresTpchQ14});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
