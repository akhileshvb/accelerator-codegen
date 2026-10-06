// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q1 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q1/postgres/aux/module.hpp"
#include "tpch/q1/layout.hpp"
#include <stdexcept>


#include "accelerator_codegen/stateful/core/decimal_text_g1.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, l_tax, "
      "l_returnflag, l_linestatus FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 4), 2,
                  TypedChar(tuple, tuple_desc, 5),
                  TypedChar(tuple, tuple_desc, 6)});
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
              "config/q1_aux_cost_model.json",
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ1(FunctionCallInfo fcinfo) {
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
