// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q11 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q11/postgres/aux/module.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ11Partsupp(
    const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2});
      });
}

void ScanTpchQ11Suppliers(
    const std::function<void(const query::TpchQ11SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ11Nations(
    const std::function<void(const query::TpchQ11NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ11Scan final : public query::TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const std::function<void(const query::TpchQ11PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ11Partsupp(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ11SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ11Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ11NationInput &)> &consumer)
      override {
    scans::ScanTpchQ11Nations(consumer);
  }
};
Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan> &
Q11Runtime() {
  static Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q11_COST_MODEL",
              "config/q11_aux_cost_model.json",
              query::TpchQ11Layout::kAccelerator,
              query::TpchQ11Config{pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ11(FunctionCallInfo fcinfo) {
  // The SQL function is not STRICT: a NULL argument reaches the module as a
  // NULL AuxValue.  The fraction arrives as numeric (passed on as its exact
  // decimal text) or as float8.
  core::AuxValue nation =
      PG_ARGISNULL(0)
          ? core::AuxValue::Null(core::AuxType::String)
          : core::AuxValue::Of(core::AuxType::String,
                               detail::TextDatum(PG_GETARG_DATUM(0)));
  core::AuxValue fraction = core::AuxValue::Null(core::AuxType::String);
  if (!PG_ARGISNULL(1)) {
    if (get_fn_expr_argtype(fcinfo->flinfo, 1) == NUMERICOID) {
      char *text = DatumGetCString(
          DirectFunctionCall1(numeric_out, PG_GETARG_DATUM(1)));
      fraction = core::AuxValue::Of(core::AuxType::String, std::string(text));
      pfree(text);
    } else {
      fraction = core::AuxValue::Of(core::AuxType::Double, PG_GETARG_FLOAT8(1));
    }
  }
  detail::Q11Runtime().Execute(
      core::QueryArguments{{std::move(nation), std::move(fraction)}}, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 11, kPostgresVariantAux, &ExecutePostgresTpchQ11});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
