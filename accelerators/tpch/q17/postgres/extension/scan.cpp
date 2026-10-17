// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q17 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q17/postgres/aux/module.hpp"
#include "tpch/q17/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ17Parts(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ17PartInput &)> &sink) {
  ScanPartitioned(services, "part",
                  {IntColumn("p_partkey"), TextColumn("p_brand"),
                   TextColumn("p_container")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2)});
                    }
                  });
}

void ScanTpchQ17Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ17LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_partkey"), DecimalColumn("l_quantity", 2),
                   DecimalColumn("l_extendedprice", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Decimal(1), row.Decimal(2)});
                    }
                  });
}

} // namespace scans

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ17PartInput &)> &sink)
      override {
    scans::ScanTpchQ17Parts(services, partitions, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ17LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ17Lineitems(services, partitions, sink);
  }
};
Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q17_COST_MODEL",
              core::kTpchQ17CostModel,
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{pg_tbl::kPart, pg_tbl::kLineitem},
              {{"part", pg_tbl::kPart}, {"lineitem", pg_tbl::kLineitem}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ17(FunctionCallInfo fcinfo) {
  // SQL NULL arguments are passed on as NULL (p_brand = NULL matches nothing).
  core::QueryArguments arguments;
  for (int i = 0; i < 2; ++i)
    arguments.values.push_back(
        PG_ARGISNULL(i) ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(
                              core::AuxType::String,
                              detail::TextDatum(PG_GETARG_DATUM(i))));
  detail::Q17Runtime().Execute(std::move(arguments), fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 17, kPostgresVariantAux, &ExecutePostgresTpchQ17});
const detail::RuntimeRegistrar<&detail::Q17Runtime> kAcceleratorRegistrar(
    query::TpchQ17Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
