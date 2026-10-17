// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q15 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q15/postgres/aux/module.hpp"
#include "tpch/q15/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ15Suppliers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ15SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier",
                  {IntColumn("s_suppkey"), TextColumn("s_name"),
                   TextColumn("s_address"), TextColumn("s_phone")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2), row.Text(3)});
                    }
                  });
}

void ScanTpchQ15Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ15LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_suppkey"), DecimalColumn("l_extendedprice", 2),
                   DecimalColumn("l_discount", 2), DateColumn("l_shipdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Date(3), row.Int(0), row.Decimal(1), 2,
                                       row.Decimal(2), 2});
                    }
                  });
}

} // namespace scans

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ15LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ15Lineitems(services, partitions, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ15SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ15Suppliers(services, partitions, sink);
  }
};
Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q15_COST_MODEL",
              core::kTpchQ15CostModel,
              query::TpchQ15Layout::kAccelerator,
              query::TpchQ15Config{pg_tbl::kLineitem, pg_tbl::kSupplier},
              {{"lineitem", pg_tbl::kLineitem}, {"supplier", pg_tbl::kSupplier}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ15(FunctionCallInfo fcinfo) {
  // A NULL date is passed on as NULL: the window is empty, as in SQL.
  detail::Q15Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 15, kPostgresVariantAux, &ExecutePostgresTpchQ15});
const detail::RuntimeRegistrar<&detail::Q15Runtime> kAcceleratorRegistrar(
    query::TpchQ15Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
