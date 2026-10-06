// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q21 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q21/postgres/aux/module.hpp"



#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ21Suppliers(
    const std::function<void(const query::TpchQ21SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Orders(
    const std::function<void(const query::TpchQ21OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedChar(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Lineitems(
    const std::function<void(const query::TpchQ21LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
      "FROM lineitem l "
      "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
      "o.o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3)});
      });
}

} // namespace scans

class TpchQ21Scan final : public query::TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ21SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ21Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ21NationInput &)> &consumer)
      override {
    scans::ScanTpchQ21Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ21OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ21Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ21LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ21Lineitems(consumer);
  }
};
Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", "s_name"}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ21(FunctionCallInfo fcinfo) {
  detail::Q21Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 21, kPostgresVariantAux, &ExecutePostgresTpchQ21});


} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
