// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q1 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q1/postgres/aux/module.hpp"
#include "tpch/q1/cost_model.hpp"
#include "tpch/q1/layout.hpp"
#include <stdexcept>


#include "prims/decimal_text.hpp"
#include "prims/sql_date.hpp"
#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanColumns("lineitem",
              {DateColumn("l_shipdate"), DecimalColumn("l_quantity", 2),
               DecimalColumn("l_extendedprice", 2), DecimalColumn("l_discount", 2),
               DecimalColumn("l_tax", 2), TextColumn("l_returnflag"),
               TextColumn("l_linestatus")},
              [&](const BuildRow &row) {
                consumer({row.Date(0), row.Decimal(1), 2, row.Decimal(2), 2,
                          row.Decimal(3), 2, row.Decimal(4), 2, row.Char(5),
                          row.Char(6)});
              });
}

// One SQL GROUP BY: the engine aggregates, the process receives one row per
// (shipdate, returnflag, linestatus) cell with exact numeric sums (text).
void ScanTpchQ1DayGroupAggregates(
    const std::function<void(const query::TpchQ1DayGroupAggregate &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_returnflag, l_linestatus, count(*), "
      "sum(l_quantity), sum(l_extendedprice), sum(l_discount), "
      "sum(l_extendedprice * (1 - l_discount)), "
      "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) "
      "FROM lineitem GROUP BY l_shipdate, l_returnflag, l_linestatus",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ1DayGroupAggregate agg;
        agg.shipdate_days = TypedDate(tuple, tuple_desc, 0);
        agg.returnflag = TypedChar(tuple, tuple_desc, 1);
        agg.linestatus = TypedChar(tuple, tuple_desc, 2);
        agg.count = TypedInt(tuple, tuple_desc, 3);
        const auto sum = [&](int attribute, int scale) {
          __int128 value = 0;
          if (!accelerator_codegen::aux_state::core::decimal_text::Parse(TypedText(tuple, tuple_desc, attribute),
                                         scale, value))
            throw std::runtime_error("TPC-H Q1: unparsable numeric aggregate");
          return value;
        };
        agg.sum_qty_raw = sum(4, 2);
        agg.sum_base_price_raw = sum(5, 2);
        agg.sum_discount_raw = sum(6, 2);
        agg.sum_disc_price_raw = sum(7, 4);
        agg.sum_charge_raw = sum(8, 6);
        consumer(agg);
      });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
  void ScanDayGroupAggregates(
      const std::function<void(const query::TpchQ1DayGroupAggregate &)>
          &consumer) override {
    scans::ScanTpchQ1DayGroupAggregates(consumer);
  }
};
Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q1_COST_MODEL",
              core::kTpchQ1CostModel,
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ1(FunctionCallInfo fcinfo) {
  // DATE '1998-12-01' - INTERVAL 'DELTA' DAY is a timestamp (4713 BC ..
  // 294276 AD): outside it PostgreSQL raises an error, so do we.
  namespace sd = accelerator_codegen::aux_state::core::sql_date;
  if (!PG_ARGISNULL(0)) {
    const int64_t cutoff = sd::DaysFromCivil(1998, 12, 1) -
                           static_cast<int64_t>(PG_GETARG_INT32(0));
    if (cutoff < sd::DaysFromCivil(-4712, 11, 24) ||
        cutoff > sd::DaysFromCivil(294276, 12, 31))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }
  // A NULL DELTA makes the predicate NULL: no row qualifies.
  detail::Q1Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Int64)
                                : core::AuxValue::Of(
                                      core::AuxType::Int64,
                                      static_cast<int64_t>(PG_GETARG_INT32(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 1, kPostgresVariantAux, &ExecutePostgresTpchQ1});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
