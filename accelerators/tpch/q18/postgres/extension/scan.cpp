// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q18 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q18/postgres/aux/module.hpp"
#include "tpch/q18/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ18CustomersSerial(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanColumns("customer",
              {IntColumn("c_custkey"), TextColumn("c_name")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Text(1)});
              });
}

void ScanTpchQ18Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ18LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), DecimalColumn("l_quantity", 2)}, partitions,
                  [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Decimal(1)});
                    }
                  });
}

void ScanTpchQ18Orders(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ18OrderInput &)> &sink) {
  ScanPartitioned(services, "orders",
                  {IntColumn("o_orderkey"), IntColumn("o_custkey"),
                   DateColumn("o_orderdate"), DecimalColumn("o_totalprice", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Date(2), row.Decimal(3)});
                    }
                  });
}

void ScanTpchQ18Customers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ18CustomerInput &)> &sink) {
  ScanPartitioned(services, "customer",
                  {IntColumn("c_custkey"), TextColumn("c_name")}, partitions,
                  [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1)});
                    }
                  });
}

// Keyed lookup of customer names (the index on c_custkey answers it).
void LookupTpchQ18CustomerNames(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name FROM customer WHERE " +
          KeyListPredicate("c_custkey", custkeys),
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ18Scan final : public query::TpchQ18InitialScan {
public:
  void ScanCustomersSerial(
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ18CustomersSerial(consumer);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ18LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ18Lineitems(services, partitions, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ18OrderInput &)> &sink)
      override {
    scans::ScanTpchQ18Orders(services, partitions, sink);
  }
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ18CustomerInput &)> &sink)
      override {
    scans::ScanTpchQ18Customers(services, partitions, sink);
  }
  void LookupCustomerNames(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    // A big key list is cheaper as one scan than as an array literal.
    if (custkeys.size() > 4096)
      query::TpchQ18InitialScan::LookupCustomerNames(custkeys, consumer);
    else
      scans::LookupTpchQ18CustomerNames(custkeys, consumer);
  }
};
Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q18_COST_MODEL",
              core::kTpchQ18CostModel,
              query::TpchQ18Layout::kAccelerator,
              query::TpchQ18Config{pg_tbl::kLineitem, pg_tbl::kOrders, pg_tbl::kCustomer},
              {{"lineitem", pg_tbl::kLineitem}, {"orders", pg_tbl::kOrders}, {"customer", pg_tbl::kCustomer}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ18(FunctionCallInfo fcinfo) {
  // QUANTITY is a NUMERIC: passed as its exact text (NULL stays NULL).
  core::QueryArguments arguments;
  if (PG_ARGISNULL(0))
    arguments.values.push_back(core::AuxValue::Null(core::AuxType::String));
  else
    arguments.values.push_back(core::AuxValue::Of(
        core::AuxType::String,
        detail::DatumText(PG_GETARG_DATUM(0), NUMERICOID)));
  detail::Q18Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 18, kPostgresVariantAux, &ExecutePostgresTpchQ18});
const detail::RuntimeRegistrar<&detail::Q18Runtime> kAcceleratorRegistrar(
    query::TpchQ18Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
