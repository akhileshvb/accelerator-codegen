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

void ScanTpchQ10Customers(
    const std::function<void(const query::TpchQ10CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name, c_acctbal, c_nationkey, c_address, "
      "c_phone, c_comment FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3)),
                  TypedText(tuple, tuple_desc, 4),
                  TypedText(tuple, tuple_desc, 5),
                  TypedText(tuple, tuple_desc, 6)});
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

void ScanTpchQ10Orders(
    const std::function<void(const query::TpchQ10OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ10Lineitems(
    const std::function<void(const query::TpchQ10LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_returnflag "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedText(tuple, tuple_desc, 3) == "R"});
      });
}

} // namespace scans

class TpchQ10Scan final : public query::TpchQ10InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ10CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ10Customers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ10NationInput &)> &consumer)
      override {
    scans::ScanTpchQ10Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ10OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ10Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ10LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ10Lineitems(consumer);
  }
};
Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL",
              core::kTpchQ10CostModel,
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{pg_tbl::kCustomer, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kLineitem});
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
