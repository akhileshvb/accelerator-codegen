// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q15 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q15/postgres/aux/module.hpp"
#include "tpch/q15/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ15Suppliers(
    const std::function<void(const query::TpchQ15SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedText(tuple, tuple_desc, 3)});
      });
}

void ScanTpchQ15Lineitems(
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedDate(tuple, tuple_desc, 3),
                  TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1), 2,
                  TypedDecimalRaw(tuple, tuple_desc, 2), 2});
      });
}

void ScanTpchQ15LineitemsForShipdateRange(
    int32_t start_days, int32_t end_days,
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  if (end_days <= start_days) {
    return;
  }
  const std::string sql =
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
      "WHERE l_shipdate >= (DATE '1970-01-01' + " +
      std::to_string(start_days) +
      ") AND l_shipdate < (DATE '1970-01-01' + " + std::to_string(end_days) +
      ")";
  ScanTypedTuples(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
    consumer({TypedDate(tuple, tuple_desc, 3), TypedInt(tuple, tuple_desc, 0),
              TypedDecimalRaw(tuple, tuple_desc, 1), 2,
              TypedDecimalRaw(tuple, tuple_desc, 2), 2});
  });
}

} // namespace scans

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15Lineitems(consumer);
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15LineitemsForShipdateRange(start_days, end_days, consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ15SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ15Suppliers(consumer);
  }
};
Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q15_COST_MODEL",
              core::kTpchQ15CostModel,
              query::TpchQ15Layout::kAccelerator,
              query::TpchQ15Config{pg_tbl::kLineitem, pg_tbl::kSupplier});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ15(FunctionCallInfo fcinfo) {
  // A NULL date is passed on as NULL: the window is empty, as in SQL.
  detail::Q15Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(0, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 15, kPostgresVariantAux, &ExecutePostgresTpchQ15});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
