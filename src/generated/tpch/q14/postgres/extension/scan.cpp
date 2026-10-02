// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q14 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q14/postgres/aux/module.hpp"
#include "tpch/q14/layout.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ14Parts(
    const std::function<void(const query::TpchQ14PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_type FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ14Lineitems(
    const std::function<void(const query::TpchQ14LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDate(tuple, tuple_desc, 3)});
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
  void ScanLineitems(
      const std::function<void(const query::TpchQ14LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ14Lineitems(consumer);
  }
};
Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan> &
Q14Runtime() {
  static Runtime<query::TpchQ14Module, query::TpchQ14Config, TpchQ14Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q14_COST_MODEL",
              "config/q14_aux_cost_model.json",
              core::TpchQ14Layout::kAccelerator, query::TpchQ14Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ14(FunctionCallInfo fcinfo) {
  detail::Q14Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Date, detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 14, kPostgresVariantAux, &ExecutePostgresTpchQ14});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
