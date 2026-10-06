// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q22 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q22/postgres/aux/module.hpp"


#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ22Customers(
    const std::function<void(const query::TpchQ22CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_phone, c_acctbal FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2});
      });
}

void ScanTpchQ22Orders(
    const std::function<void(const query::TpchQ22OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0)});
      });
}

// Orders per customer, aggregated in SQL (one row per o_custkey).
void ScanTpchQ22OrderCounts(
    const std::function<void(const query::TpchQ22OrderCount &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey, COUNT(*)::BIGINT FROM orders "
      "WHERE o_custkey IS NOT NULL GROUP BY o_custkey",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ22CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ22Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ22OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ22Orders(consumer);
  }
  bool ScanOrderCounts(
      const std::function<void(const query::TpchQ22OrderCount &)> &consumer)
      override {
    scans::ScanTpchQ22OrderCounts(consumer);
    return true;
  }
};
Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q22_COST_MODEL",
              "config/q22_aux_cost_model.json",
              query::TpchQ22Layout::kAccelerator,
              query::TpchQ22Config{pg_tbl::kOrders, pg_tbl::kCustomer});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ22(FunctionCallInfo fcinfo) {
  // SQL NULL codes are passed on as NULL (they never match a phone prefix).
  core::QueryArguments arguments;
  for (int i = 0; i < 7; ++i)
    arguments.values.push_back(
        PG_ARGISNULL(i) ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(
                              core::AuxType::String,
                              detail::TextDatum(PG_GETARG_DATUM(i))));
  detail::Q22Runtime().Execute(std::move(arguments), fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 22, kPostgresVariantAux, &ExecutePostgresTpchQ22});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
