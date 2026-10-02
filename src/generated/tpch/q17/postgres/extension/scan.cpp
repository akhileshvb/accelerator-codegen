// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q17 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q17/postgres/aux/module.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

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

void ScanTpchQ17PartQuantityAggregatesForPart(
    int64_t partkey,
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  const std::string sql =
      "SELECT CAST(round(l_quantity * 100) AS BIGINT), "
      "SUM(CAST(round(l_extendedprice * 100) AS BIGINT)), "
      "COUNT(*)::BIGINT "
      "FROM lineitem WHERE l_partkey = " +
      std::to_string(partkey) + " GROUP BY 1";
  ScanTypedTuples(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
    consumer({partkey, TypedInt(tuple, tuple_desc, 0),
              TypedInt(tuple, tuple_desc, 1), TypedInt(tuple, tuple_desc, 2)});
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
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregatesForPart(partkey, consumer);
    return true;
  }
};
Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q17_COST_MODEL",
              "config/q17_aux_cost_model.json",
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{pg_tbl::kPart, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ17(FunctionCallInfo fcinfo) {
  detail::Q17Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(1)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 17, kPostgresVariantAux, &ExecutePostgresTpchQ17});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
