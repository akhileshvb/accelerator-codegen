// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q18 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q18/postgres/aux/module.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ18Lineitems(
    const std::function<void(const query::TpchQ18LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_quantity FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ18Orders(
    const std::function<void(const query::TpchQ18OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ18Customers(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

bool LookupTpchQ18Order(int64_t orderkey, query::TpchQ18OrderInput &out) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    return false;
  }
  char sql[256];
  snprintf(sql, sizeof(sql),
                "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice "
                "FROM orders WHERE o_orderkey = %lld",
                static_cast<long long>(orderkey));
  ++t_table_scans;
  const int status = SPI_execute(sql, true, 1);
  bool found = false;
  if (status == SPI_OK_SELECT && SPI_processed > 0 && SPI_tuptable) {
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tuple_desc = SPI_tuptable->tupdesc;
    out = {TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
           TypedDate(tuple, tuple_desc, 2),
           TypedDecimalRaw(tuple, tuple_desc, 3)};
    found = out.key == orderkey;
  }
  SPI_finish();
  return found;
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
  bool LookupOrder(int64_t orderkey,
                   query::TpchQ18OrderInput &out) override {
    // SPI is unsafe inside PostgreSQL xact callbacks (PreCommit maintenance).
    // Returning false makes StageMaintenance fall back to RebuildAll.
    (void)orderkey;
    (void)out;
    return false;
  }
};
Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q18_COST_MODEL",
              "config/q18_aux_cost_model.json",
              query::TpchQ18Layout::kAccelerator,
              query::TpchQ18Config{pg_tbl::kLineitem, pg_tbl::kOrders, pg_tbl::kCustomer});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ18(FunctionCallInfo fcinfo) {
  detail::Q18Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(PG_GETARG_INT64(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 18, kPostgresVariantAux, &ExecutePostgresTpchQ18});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
