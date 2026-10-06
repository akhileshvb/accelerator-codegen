// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q13 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q13/postgres/aux/module.hpp"
#include "tpch/q13/layout.hpp"


#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ13Customers(
    const std::function<void(const query::TpchQ13CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey FROM customer WHERE c_custkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0)});
      });
}

void ScanTpchQ13Orders(
    const std::function<void(const query::TpchQ13OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_comment FROM orders "
      "WHERE o_orderkey IS NOT NULL AND o_custkey IS NOT NULL AND o_comment IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2)});
      });
}

} // namespace scans

class TpchQ13Scan final : public query::TpchQ13InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ13CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ13Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ13OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ13Orders(consumer);
  }
};
Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan> &
Q13Runtime() {
  static Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q13_COST_MODEL",
              "config/q13_aux_cost_model.json",
              core::TpchQ13Layout::kAccelerator, query::TpchQ13Config{pg_tbl::kCustomer, pg_tbl::kOrders});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ13(FunctionCallInfo fcinfo) {
  detail::Q13Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::String)
                                : core::AuxValue::Of(
                                      core::AuxType::String,
                                      detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 13, kPostgresVariantAux, &ExecutePostgresTpchQ13});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
