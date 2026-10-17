// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q22 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q22/postgres/aux/module.hpp"
#include "tpch/q22/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ22Customers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ22CustomerInput &)> &sink) {
  ScanPartitioned(services, "customer",
                  {IntColumn("c_custkey"), TextColumn("c_phone"),
                   DecimalColumn("c_acctbal", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Decimal(2), 2});
                    }
                  });
}

void ScanTpchQ22Orders(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ22OrderInput &)> &sink) {
  ScanPartitioned(services, "orders", {IntColumn("o_custkey")}, partitions,
                  [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i)
                      sink(partition, {batch.Row(i).Int(0)});
                  });
}

} // namespace scans

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ22CustomerInput &)> &sink)
      override {
    scans::ScanTpchQ22Customers(services, partitions, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ22OrderInput &)> &sink)
      override {
    scans::ScanTpchQ22Orders(services, partitions, sink);
  }
};
Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q22_COST_MODEL",
              core::kTpchQ22CostModel,
              query::TpchQ22Layout::kAccelerator,
              query::TpchQ22Config{pg_tbl::kOrders, pg_tbl::kCustomer},
              {{"orders", pg_tbl::kOrders}, {"customer", pg_tbl::kCustomer}});
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
const detail::RuntimeRegistrar<&detail::Q22Runtime> kAcceleratorRegistrar(
    query::TpchQ22Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
