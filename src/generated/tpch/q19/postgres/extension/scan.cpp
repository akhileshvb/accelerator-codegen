// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q19 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q19/postgres/aux/module.hpp"


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ19Lineitems(
    const std::function<void(const query::TpchQ19LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice, l_discount, l_shipmode, "
      "l_shipinstruct FROM lineitem",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDecimalRaw(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3),
                  TypedText(tuple, tuple_desc, 4),
                  TypedText(tuple, tuple_desc, 5)});
      });
}

bool LookupTpchQ19Part(int64_t partkey, query::TpchQ19PartInput &out) {
  if (SPI_connect() != SPI_OK_CONNECT) {
    return false;
  }
  char sql[256];
  snprintf(sql, sizeof(sql),
           "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = %lld",
           static_cast<long long>(partkey));
  ++t_table_scans;
  const int status = SPI_execute(sql, true, 1);
  bool found = false;
  if (status == SPI_OK_SELECT && SPI_processed > 0 && SPI_tuptable) {
    HeapTuple tuple = SPI_tuptable->vals[0];
    TupleDesc tuple_desc = SPI_tuptable->tupdesc;
    out = {partkey, TypedText(tuple, tuple_desc, 0),
           TypedText(tuple, tuple_desc, 1),
           static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))};
    found = true;
  }
  SPI_finish();
  return found;
}

} // namespace scans

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ19LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ19Lineitems(consumer);
  }
  bool LookupPart(int64_t partkey, query::TpchQ19PartInput &out) override {
    return scans::LookupTpchQ19Part(partkey, out);
  }
};
Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q19_COST_MODEL",
              "config/q19_aux_cost_model.json",
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{pg_tbl::kLineitem, pg_tbl::kPart});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ19(FunctionCallInfo fcinfo) {
  detail::Q19Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(0))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(1)),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(2))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(3)),
                            core::AuxValue::Of(
                                core::AuxType::String,
                                detail::TextDatum(PG_GETARG_DATUM(4))),
                            core::AuxValue::Of(core::AuxType::Int64,
                                               PG_GETARG_INT64(5))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 19, kPostgresVariantAux, &ExecutePostgresTpchQ19});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
