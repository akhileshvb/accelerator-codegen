// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q6 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q6/postgres/aux/module.hpp"
#include "tpch/q6/layout.hpp"


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

} // namespace scans

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
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
  detail::Q6Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(0, fcinfo)),
                            core::AuxValue::Of(core::AuxType::Double,
                                               PG_GETARG_FLOAT8(1)),
                            core::AuxValue::Of(core::AuxType::Double,
                                               PG_GETARG_FLOAT8(2))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 6, kPostgresVariantAux, &ExecutePostgresTpchQ6});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
