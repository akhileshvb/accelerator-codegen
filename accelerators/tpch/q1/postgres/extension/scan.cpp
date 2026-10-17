// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q1 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q1/postgres/aux/module.hpp"
#include "tpch/q1/cost_model.hpp"
#include "tpch/q1/layout.hpp"
#include <stdexcept>


#include "prims/decimal_text.hpp"
#include "prims/sql_date.hpp"
#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ1Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ1LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {DateColumn("l_shipdate"), DecimalColumn("l_quantity", 2),
                   DecimalColumn("l_extendedprice", 2), DecimalColumn("l_discount", 2),
                   DecimalColumn("l_tax", 2), TextColumn("l_returnflag"),
                   TextColumn("l_linestatus")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Date(0), row.Decimal(1), 2, row.Decimal(2), 2,
                                       row.Decimal(3), 2, row.Decimal(4), 2, row.Char(5),
                                       row.Char(6)});
                    }
                  });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ1LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ1Lineitems(services, partitions, sink);
  }
};
Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q1_COST_MODEL",
              core::kTpchQ1CostModel,
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{pg_tbl::kLineitem},
              {{"lineitem", pg_tbl::kLineitem}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ1(FunctionCallInfo fcinfo) {
  // DATE '1998-12-01' - INTERVAL 'DELTA' DAY is a timestamp (4713 BC ..
  // 294276 AD): outside it PostgreSQL raises an error, so do we.
  namespace sd = accelerator_codegen::aux_state::core::sql_date;
  if (!PG_ARGISNULL(0)) {
    const int64_t cutoff = sd::DaysFromCivil(1998, 12, 1) -
                           static_cast<int64_t>(PG_GETARG_INT32(0));
    if (cutoff < sd::DaysFromCivil(-4712, 11, 24) ||
        cutoff > sd::DaysFromCivil(294276, 12, 31))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }
  // A NULL DELTA makes the predicate NULL: no row qualifies.
  detail::Q1Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Int64)
                                : core::AuxValue::Of(
                                      core::AuxType::Int64,
                                      static_cast<int64_t>(PG_GETARG_INT32(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 1, kPostgresVariantAux, &ExecutePostgresTpchQ1});
const detail::RuntimeRegistrar<&detail::Q1Runtime> kAcceleratorRegistrar(
    core::TpchQ1Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
