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
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ4OrderInput &)> &sink) {
  ScanPartitioned(services, "orders",
                  {IntColumn("o_orderkey"), DateColumn("o_orderdate"),
                   TextColumn("o_orderpriority")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Date(1), row.Text(2), false});
                    }
                  });
}

void ScanTpchQ4Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ4LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), DateColumn("l_commitdate"),
                   DateColumn("l_receiptdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Date(1), row.Date(2)});
                    }
                  });
}

} // namespace scans

class TpchQ4Scan final : public query::TpchQ4InitialScan {
public:
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ4OrderInput &)> &sink)
      override {
    scans::ScanTpchQ4Orders(services, partitions, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ4LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ4Lineitems(services, partitions, sink);
  }
};
Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan> &Q4Runtime() {
  static Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q4_COST_MODEL",
              core::kTpchQ4CostModel, query::TpchQ4Layout::kAccelerator,
              query::TpchQ4Config{pg_tbl::kOrders, pg_tbl::kLineitem},
              {{"orders", pg_tbl::kOrders}, {"lineitem", pg_tbl::kLineitem}});
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
