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

void ScanTpchQ7Suppliers(
    const std::function<void(const query::TpchQ7SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ7Customers(
    const std::function<void(const query::TpchQ7CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ7Orders(
    const std::function<void(const query::TpchQ7OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey FROM orders",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ7Lineitems(
    const std::function<void(const query::TpchQ7LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
      "FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 3), 2,
                  TypedDate(tuple, tuple_desc, 4)});
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
  void ScanSuppliers(
      const std::function<void(const query::TpchQ7SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ7Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ7CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ7Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ7OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ7Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ7LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ7Lineitems(consumer);
  }
};
Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan> &Q7Runtime() {
  static Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q7_COST_MODEL",
              core::kTpchQ7CostModel, core::TpchQ7Layout::kAccelerator,
              query::TpchQ7Config{pg_tbl::kNation, pg_tbl::kSupplier, pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem},
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

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
