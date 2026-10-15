// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q4 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q4/postgres/aux/module.hpp"
#include "tpch/q4/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ4Orders(
    const std::function<void(const query::TpchQ4OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_orderpriority, "
      "(o_orderpriority IS NULL)::int FROM orders "
      "WHERE o_orderkey IS NOT NULL AND o_orderdate IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedInt(tuple, tuple_desc, 3) != 0});
      });
}

void ScanTpchQ4Lineitems(
    const std::function<void(const query::TpchQ4LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_commitdate, l_receiptdate FROM lineitem "
      "WHERE l_orderkey IS NOT NULL AND l_commitdate < l_receiptdate",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

} // namespace scans

class TpchQ4Scan final : public query::TpchQ4InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ4OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ4Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ4LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ4Lineitems(consumer);
  }
};
Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan> &Q4Runtime() {
  static Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q4_COST_MODEL",
              core::kTpchQ4CostModel, query::TpchQ4Layout::kAccelerator,
              query::TpchQ4Config{pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ4(FunctionCallInfo fcinfo) {
  // SQL NULL arguments reach the module as NULL values.
  detail::Q4Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 4, kPostgresVariantAux, &ExecutePostgresTpchQ4});
const detail::RuntimeRegistrar<&detail::Q4Runtime> kAcceleratorRegistrar(
    query::TpchQ4Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
