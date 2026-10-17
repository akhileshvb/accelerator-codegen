// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q7 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q7/postgres/aux/module.hpp"
#include "tpch/q7/cost_model.hpp"
#include "tpch/q7/layout.hpp"


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

void ScanTpchQ7Nations(
    const std::function<void(const query::TpchQ7NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ7SuppliersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ7SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier", {IntColumn("s_suppkey"), IntColumn("s_nationkey")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), static_cast<int32_t>(row.Int(1))});
                    }
                  });
}

void ScanTpchQ7CustomersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ7CustomerInput &)> &sink) {
  ScanPartitioned(services, "customer", {IntColumn("c_custkey"), IntColumn("c_nationkey")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), static_cast<int32_t>(row.Int(1))});
                    }
                  });
}

void ScanTpchQ7OrdersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ7OrderInput &)> &sink) {
  ScanPartitioned(services, "orders", {IntColumn("o_orderkey"), IntColumn("o_custkey")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1)});
                    }
                  });
}

void ScanTpchQ7LineitemsPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ7LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), IntColumn("l_suppkey"),
                   DecimalColumn("l_extendedprice", 2), DecimalColumn("l_discount", 2),
                   DateColumn("l_shipdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Decimal(2), 2, row.Decimal(3),
                                       2, row.Date(4)});
                    }
                  });
}

} // namespace scans

class TpchQ7Scan final : public query::TpchQ7InitialScan {
public:
  void ScanNations(
      const std::function<void(const query::TpchQ7NationInput &)> &consumer)
      override {
    scans::ScanTpchQ7Nations(consumer);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ7SuppliersPartitioned(services, partitions, sink);
  }
  void ScanCustomersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7CustomerInput &)> &sink)
      override {
    scans::ScanTpchQ7CustomersPartitioned(services, partitions, sink);
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7OrderInput &)> &sink)
      override {
    scans::ScanTpchQ7OrdersPartitioned(services, partitions, sink);
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ7LineitemsPartitioned(services, partitions, sink);
  }
};
Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan> &Q7Runtime() {
  static Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q7_COST_MODEL",
              core::kTpchQ7CostModel, core::TpchQ7Layout::kAccelerator,
              query::TpchQ7Config{pg_tbl::kNation, pg_tbl::kSupplier, pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem},
              {{"nation", pg_tbl::kNation}, {"supplier", pg_tbl::kSupplier}, {"customer", pg_tbl::kCustomer}, {"orders", pg_tbl::kOrders}, {"lineitem", pg_tbl::kLineitem}},
              {{"nation", "n_name"}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ7(FunctionCallInfo fcinfo) {
  RunOrReport([&] {
    detail::Q7Runtime().Execute(
        core::QueryArguments{{TextArg(fcinfo, 0), TextArg(fcinfo, 1)}}, fcinfo);
  });
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 7, kPostgresVariantAux, &ExecutePostgresTpchQ7});
const detail::RuntimeRegistrar<&detail::Q7Runtime> kAcceleratorRegistrar(
    core::TpchQ7Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
