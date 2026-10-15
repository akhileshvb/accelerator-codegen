// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q8 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q8/postgres/aux/module.hpp"
#include "tpch/q8/cost_model.hpp"


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

void ScanTpchQ8Parts(
    const std::function<void(const query::TpchQ8PartInput &)> &consumer) {
  ScanColumns("part",
              {IntColumn("p_partkey"), TextColumn("p_type")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Text(1)});
              });
}

void ScanTpchQ8Suppliers(
    const std::function<void(const query::TpchQ8SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ8Customers(
    const std::function<void(const query::TpchQ8CustomerInput &)> &consumer) {
  ScanColumns("customer",
              {IntColumn("c_custkey"), IntColumn("c_nationkey")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          static_cast<int32_t>(row.Int(1))});
              });
}

void ScanTpchQ8Orders(
    const std::function<void(const query::TpchQ8OrdersInput &)> &consumer) {
  ScanColumns("orders",
              {IntColumn("o_orderkey"), IntColumn("o_custkey"),
               DateColumn("o_orderdate")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Int(1),
                          row.Date(2)});
              });
}

void ScanTpchQ8Lineitems(
    const std::function<void(const query::TpchQ8LineitemInput &)> &consumer) {
  ScanColumns("lineitem",
              {IntColumn("l_partkey"), IntColumn("l_orderkey"),
               IntColumn("l_suppkey"), DecimalColumn("l_extendedprice", 2),
               DecimalColumn("l_discount", 2)},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Int(1),
                          row.Int(2),
                          row.Decimal(3),
                          row.Decimal(4)});
              });
}

void ScanTpchQ8Nations(
    const std::function<void(const query::TpchQ8NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ8Regions(
    const std::function<void(const query::TpchQ8RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ8Scan final : public query::TpchQ8InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ8PartInput &)> &consumer)
      override {
    scans::ScanTpchQ8Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ8SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ8Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ8CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ8Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ8OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ8Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ8LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ8Lineitems(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ8NationInput &)> &consumer)
      override {
    scans::ScanTpchQ8Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ8RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ8Regions(consumer);
  }
};
Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan> &Q8Runtime() {
  static Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q8_COST_MODEL",
              core::kTpchQ8CostModel, query::TpchQ8Layout::kAccelerator,
              query::TpchQ8Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kCustomer, pg_tbl::kOrders, pg_tbl::kLineitem, pg_tbl::kNation, pg_tbl::kRegion});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ8(FunctionCallInfo fcinfo) {
  RunOrReport([&] {
    detail::Q8Runtime().Execute(
        core::QueryArguments{{TextArg(fcinfo, 0), TextArg(fcinfo, 1),
                              TextArg(fcinfo, 2)}},
        fcinfo);
  });
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 8, kPostgresVariantAux, &ExecutePostgresTpchQ8});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
