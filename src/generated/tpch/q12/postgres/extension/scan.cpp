// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q12 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q12/postgres/aux/module.hpp"
#include "tpch/q12/layout.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderpriority FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3),
                  TypedDate(tuple, tuple_desc, 4)});
      });
}

} // namespace scans

class TpchQ12Scan final : public query::TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ12OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ12Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ12Lineitems(consumer);
  }
};
Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q12_COST_MODEL",
              "config/q12_aux_cost_model.json",
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ12(FunctionCallInfo fcinfo) {
  detail::Q12Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1))),
                            core::AuxValue::Of(
                                core::AuxType::Date,
                                detail::PostgresDateArg(2, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 12, kPostgresVariantAux, &ExecutePostgresTpchQ12});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
