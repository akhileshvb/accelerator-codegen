// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q17 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q17/postgres/aux/module.hpp"
#include "tpch/q17/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ17Parts(
    const std::function<void(const query::TpchQ17PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ17Lineitems(
    const std::function<void(const query::TpchQ17LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

// GROUP BY (partkey, quantity), ordered by partkey so the module can assemble
// one part's histogram at a time. Rows with a NULL partkey or quantity never
// take part in the query.
void ScanTpchQ17PartQuantityAggregates(
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, CAST(round(l_quantity * 100) AS BIGINT) AS q, "
      "CAST(SUM(CAST(round(COALESCE(l_extendedprice, 0) * 100) AS BIGINT)) "
      "AS BIGINT), COUNT(*)::BIGINT "
      "FROM lineitem WHERE l_partkey IS NOT NULL AND l_quantity IS NOT NULL "
      "GROUP BY l_partkey, q ORDER BY l_partkey, q",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedInt(tuple, tuple_desc, 3)});
      });
}

} // namespace scans

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ17PartInput &)> &consumer)
      override {
    scans::ScanTpchQ17Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ17LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ17Lineitems(consumer);
  }
  bool ScanPartQuantityAggregates(
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregates(consumer);
    return true;
  }
};
Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q17_COST_MODEL",
              core::kTpchQ17CostModel,
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ17(FunctionCallInfo fcinfo) {
  // SQL NULL arguments are passed on as NULL (p_brand = NULL matches nothing).
  core::QueryArguments arguments;
  for (int i = 0; i < 2; ++i)
    arguments.values.push_back(
        PG_ARGISNULL(i) ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(
                              core::AuxType::String,
                              detail::TextDatum(PG_GETARG_DATUM(i))));
  detail::Q17Runtime().Execute(std::move(arguments), fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 17, kPostgresVariantAux, &ExecutePostgresTpchQ17});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
