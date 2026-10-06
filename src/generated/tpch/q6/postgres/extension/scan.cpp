// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q6 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q6/postgres/aux/module.hpp"
#include "tpch/q6/layout.hpp"
#include <stdexcept>


#include "accelerator_codegen/stateful/core/decimal_text.hpp"
#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ6Lineitems(
    const std::function<void(const query::TpchQ6LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2});
      });
}

// One SQL GROUP BY: the engine aggregates, the process receives one row per
// (shipdate, discount, quantity) cell with the exact revenue sum (text).
void ScanTpchQ6CellAggregates(
    const std::function<void(const query::TpchQ6CellAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, count(*), "
      "sum(l_extendedprice * l_discount) FROM lineitem "
      "GROUP BY l_shipdate, l_discount, l_quantity",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        query::TpchQ6CellAggregate cell;
        cell.shipdate_days = TypedDate(tuple, tuple_desc, 0);
        __int128 discount = 0, quantity = 0;
        if (!dec::Parse(TypedText(tuple, tuple_desc, 1), 2, discount) ||
            !dec::Parse(TypedText(tuple, tuple_desc, 2), 2, quantity) ||
            !dec::Parse(TypedText(tuple, tuple_desc, 4), 4, cell.revenue_raw))
          throw std::runtime_error("TPC-H Q6: unparsable numeric");
        cell.discount_raw = static_cast<int64_t>(discount);
        cell.quantity_raw = static_cast<int64_t>(quantity);
        cell.count = TypedInt(tuple, tuple_desc, 3);
        consumer(cell);
      });
}

} // namespace scans

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
  }
  void ScanCellAggregates(
      const std::function<void(const query::TpchQ6CellAggregate &)> &consumer)
      override {
    scans::ScanTpchQ6CellAggregates(consumer);
  }
};
Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL",
              "config/q6_aux_cost_model.json",
              core::TpchQ6Layout::kAccelerator, query::TpchQ6Config{pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ6(FunctionCallInfo fcinfo) {
  // A NULL argument is passed on as a NULL value (the SQL predicate is NULL).
  detail::Q6Runtime().Execute(
      core::QueryArguments{
          {PG_ARGISNULL(0) ? core::AuxValue::Null(core::AuxType::Date)
                           : core::AuxValue::Of(
                                 core::AuxType::Date,
                                 detail::PostgresDateArg(0, fcinfo)),
           PG_ARGISNULL(1) ? core::AuxValue::Null(core::AuxType::Double)
                           : core::AuxValue::Of(core::AuxType::Double,
                                                PG_GETARG_FLOAT8(1)),
           PG_ARGISNULL(2) ? core::AuxValue::Null(core::AuxType::Double)
                           : core::AuxValue::Of(core::AuxType::Double,
                                                PG_GETARG_FLOAT8(2))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 6, kPostgresVariantAux, &ExecutePostgresTpchQ6});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
