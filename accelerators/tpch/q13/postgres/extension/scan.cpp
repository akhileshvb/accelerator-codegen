// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q13 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q13/postgres/aux/module.hpp"
#include "tpch/q13/cost_model.hpp"
#include "tpch/q13/layout.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ13Customers(
    const std::function<void(const query::TpchQ13CustomerInput &)> &consumer) {
  ScanColumns("customer",
              {IntColumn("c_custkey")},
              [&](const BuildRow &row) {
                consumer({row.Int(0)});
              });
}

void ScanTpchQ13Orders(
    const std::function<void(const query::TpchQ13OrderInput &)> &consumer) {
  ScanColumns("orders",
              {IntColumn("o_orderkey"), IntColumn("o_custkey"),
               TextColumn("o_comment")},
              [&](const BuildRow &row) {
                consumer({row.Int(0), row.Int(1),
                          row.Text(2)});
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
              core::kTpchQ13CostModel,
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
const detail::RuntimeRegistrar<&detail::Q13Runtime> kAcceleratorRegistrar(
    core::TpchQ13Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
