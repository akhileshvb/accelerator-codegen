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

void ScanTpchQ18Lineitems(
    const std::function<void(const query::TpchQ18LineitemInput &)> &consumer) {
  ScanColumns("lineitem",
              {IntColumn("l_orderkey"), DecimalColumn("l_quantity", 2)},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Decimal(1)});
              });
}

void ScanTpchQ18Orders(
    const std::function<void(const query::TpchQ18OrderInput &)> &consumer) {
  ScanColumns("orders",
              {IntColumn("o_orderkey"), IntColumn("o_custkey"),
               DateColumn("o_orderdate"), DecimalColumn("o_totalprice", 2)},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Int(1),
                          row.Date(2),
                          row.Decimal(3)});
              });
}

void ScanTpchQ18Customers(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanColumns("customer",
              {IntColumn("c_custkey"), TextColumn("c_name")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Text(1)});
              });
}

// SUM(l_quantity) and COUNT(*) per order, computed by the database.
bool ScanTpchQ18OrderQuantityAggregates(
    const std::function<void(const query::TpchQ18OrderQuantityAggregate &)>
        &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, SUM(l_quantity), COUNT(*) FROM lineitem "
      "GROUP BY l_orderkey",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2)});
      });
  return true;
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
  void ScanLineitems(
      const std::function<void(const query::TpchQ18LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ18Lineitems(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ18OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ18Orders(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ18Customers(consumer);
  }
  bool ScanOrderQuantityAggregates(
      const std::function<void(const query::TpchQ18OrderQuantityAggregate &)>
          &consumer) override {
    return scans::ScanTpchQ18OrderQuantityAggregates(consumer);
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
