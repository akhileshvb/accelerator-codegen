// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q10 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q10/postgres/aux/module.hpp"
#include "tpch/q10/cost_model.hpp"
#include "tpch/q10/layout.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

#include <stdexcept>
#include <string>

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ10CustomersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ10CustomerInput &)> &sink) {
  ScanPartitioned(services, "customer",
                  {IntColumn("c_custkey"), TextColumn("c_name"),
                   DecimalColumn("c_acctbal", 2), IntColumn("c_nationkey"),
                   TextColumn("c_address"), TextColumn("c_phone"),
                   TextColumn("c_comment")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Decimal(2),
                                       static_cast<int32_t>(row.Int(3)),
                                       row.Text(4), row.Text(5), row.Text(6)});
                    }
                  });
}

void ScanTpchQ10Nations(
    const std::function<void(const query::TpchQ10NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ10OrdersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ10OrderInput &)> &sink) {
  ScanPartitioned(services, "orders",
                  {IntColumn("o_orderkey"), IntColumn("o_custkey"),
                   DateColumn("o_orderdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Date(2)});
                    }
                  });
}

void ScanTpchQ10LineitemsPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ10LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), DecimalColumn("l_extendedprice", 2),
                   DecimalColumn("l_discount", 2), TextColumn("l_returnflag")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Decimal(1), 2,
                                       row.Decimal(2), 2, row.Text(3) == "R"});
                    }
                  });
}

} // namespace scans

class TpchQ10Scan final : public query::TpchQ10InitialScan {
public:
  void ScanCustomersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10CustomerInput &)> &sink)
      override {
    scans::ScanTpchQ10CustomersPartitioned(services, partitions, sink);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ10NationInput &)> &consumer)
      override {
    scans::ScanTpchQ10Nations(consumer);
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10OrderInput &)> &sink)
      override {
    scans::ScanTpchQ10OrdersPartitioned(services, partitions, sink);
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ10LineitemsPartitioned(services, partitions, sink);
  }
};
Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL",
              core::kTpchQ10CostModel,
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{pg_tbl::kCustomer, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kLineitem},
              {{"customer", pg_tbl::kCustomer}, {"nation", pg_tbl::kNation}, {"orders", pg_tbl::kOrders}, {"lineitem", pg_tbl::kLineitem}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ10(FunctionCallInfo fcinfo) {
  // A NULL date makes the window predicate NULL (no rows).
  RunOrReport([&] {
    detail::Q10Runtime().Execute(
        core::QueryArguments{{PG_ARGISNULL(0)
                                  ? core::AuxValue::Null(core::AuxType::Date)
                                  : core::AuxValue::Of(
                                        core::AuxType::Date,
                                        detail::PostgresDateArg(0, fcinfo))}},
        fcinfo);
  });
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 10, kPostgresVariantAux, &ExecutePostgresTpchQ10});
const detail::RuntimeRegistrar<&detail::Q10Runtime> kAcceleratorRegistrar(
    core::TpchQ10Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
