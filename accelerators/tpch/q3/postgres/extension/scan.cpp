// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q3 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q3/postgres/aux/module.hpp"
#include "tpch/q3/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

// customer ⨝ orders ⨝ lineitem, one row per lineitem, one order's rows
// consecutive. `restrict_sql` is an extra predicate (empty: every order).
template <typename Scan>
void RunOrderLines(
    Scan &&scan, const std::string &restrict_sql,
    const std::function<void(const query::TpchQ3OrderLineInput &)> &consumer) {
  scan("SELECT o.o_orderkey, o.o_orderdate, o.o_shippriority, c.c_mktsegment, "
       "l.l_shipdate, l.l_extendedprice, l.l_discount "
       "FROM orders o JOIN customer c ON c.c_custkey = o.o_custkey "
       "JOIN lineitem l ON l.l_orderkey = o.o_orderkey" +
           restrict_sql + " ORDER BY o.o_orderkey",
       [&](HeapTuple tuple, TupleDesc tuple_desc) {
         query::TpchQ3OrderLineInput line;
         line.orderkey = TypedInt(tuple, tuple_desc, 0);
         line.orderdate_days = TypedDate(tuple, tuple_desc, 1);
         line.shippriority =
             static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2));
         line.mktsegment = TypedText(tuple, tuple_desc, 3);
         line.shipdate_days = TypedDate(tuple, tuple_desc, 4);
         line.extended_price_raw = TypedDecimalRaw(tuple, tuple_desc, 5);
         line.discount_raw = TypedDecimalRaw(tuple, tuple_desc, 6);
         consumer(line);
       });
}

void ScanTpchQ3Customers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ3CustomerInput &)> &sink) {
  ScanPartitioned(services, "customer", {IntColumn("c_custkey"), TextColumn("c_mktsegment")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1)});
                    }
                  });
}

void ScanTpchQ3Orders(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ3OrderInput &)> &sink) {
  ScanPartitioned(services, "orders",
                  {IntColumn("o_orderkey"), IntColumn("o_custkey"),
                   DateColumn("o_orderdate"), IntColumn("o_shippriority")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Date(2),
                                       static_cast<int32_t>(row.Int(3))});
                    }
                  });
}

void ScanTpchQ3Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ3LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), DateColumn("l_shipdate"),
                   DecimalColumn("l_extendedprice", 2), DecimalColumn("l_discount", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Date(1), row.Decimal(2), row.Decimal(3)});
                    }
                  });
}

} // namespace scans

class TpchQ3Scan final : public query::TpchQ3InitialScan {
public:
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3CustomerInput &)> &sink)
      override {
    scans::ScanTpchQ3Customers(services, partitions, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3OrderInput &)> &sink)
      override {
    scans::ScanTpchQ3Orders(services, partitions, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ3Lineitems(services, partitions, sink);
  }
  bool LookupOrderLines(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ3OrderLineInput &)> &consumer)
      override {
    // Called from the capture trigger: read the database as the writing
    // statement (and this transaction) left it.
    scans::RunOrderLines(
        [](const std::string &sql, auto &&row) {
          ScanTypedTuples(sql, row);
        },
        " WHERE " + KeyListPredicate("o.o_orderkey", orderkeys), consumer);
    return true;
  }
  bool LookupOrdersOfCustomers(const std::vector<int64_t> &custkeys,
                               std::vector<int64_t> &orderkeys) override {
    ScanTypedTuples(
        "SELECT o_orderkey FROM orders WHERE " +
            KeyListPredicate("o_custkey", custkeys),
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          orderkeys.push_back(TypedInt(tuple, tuple_desc, 0));
        });
    return true;
  }
};
Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q3_COST_MODEL",
              core::kTpchQ3CostModel,
              query::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem},
              {{"customer", pg_tbl::kCustomer}, {"orders", pg_tbl::kOrders}, {"lineitem", pg_tbl::kLineitem}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ3(FunctionCallInfo fcinfo) {
  // NULL arguments stay NULL; an infinite date keeps no row either (no
  // finite date is below -infinity or above +infinity), so it reads as NULL.
  core::QueryArguments arguments;
  if (PG_ARGISNULL(0))
    arguments.values.push_back(core::AuxValue::Null(core::AuxType::String));
  else
    arguments.values.push_back(core::AuxValue::Of(
        core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0))));
  if (PG_ARGISNULL(1) || DATE_NOT_FINITE(PG_GETARG_DATEADT(1)))
    arguments.values.push_back(core::AuxValue::Null(core::AuxType::Date));
  else
    arguments.values.push_back(core::AuxValue::Of(
        core::AuxType::Date, detail::PostgresDateArg(1, fcinfo)));
  detail::Q3Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 3, kPostgresVariantAux, &ExecutePostgresTpchQ3});
const detail::RuntimeRegistrar<&detail::Q3Runtime> kAcceleratorRegistrar(
    query::TpchQ3Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
