// GENERATED BACKEND — PostgreSQL scans + runtime for SEC-EDGAR Q3 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "sec_edgar/q3/postgres/aux/module.hpp"
#include "sec_edgar/q3/layout.hpp"

#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

// Table ids the capture triggers pass (pg/sec_edgar_mutation_triggers.sql:
// sub=1 num=2 pre=3 tag=4) and the module names in Dependencies().
constexpr core::TableId kSecEdgarSub = 1;
constexpr core::TableId kSecEdgarNum = 2;

bool IsNullAt(HeapTuple tuple, TupleDesc tuple_desc, int attribute) {
  bool is_null = false;
  SPI_getbinval(tuple, tuple_desc, attribute + 1, &is_null);
  return is_null;
}

class SecEdgarQ3PgScan final : public query::SecEdgarQ3InitialScan {
public:
  // The value filter and the per-(adsh, uom) aggregation run in the server
  // (one GROUP BY, parallel-capable); only one row per filing and unit
  // reaches the process.
  void ScanNumAggregates(
      const std::function<void(const query::SecEdgarQ3NumAggInput &)> &consumer)
      override {
    ScanTypedTuples(
        "SELECT adsh, uom, SUM(value)::float8, COUNT(*) FROM num "
        "WHERE value IS NOT NULL AND adsh IS NOT NULL AND uom IS NOT NULL "
        "GROUP BY adsh, uom",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          bool sum_null = false;
          query::SecEdgarQ3NumAggInput input;
          input.adsh = TypedText(tuple, tuple_desc, 0);
          input.uom = TypedText(tuple, tuple_desc, 1);
          input.sum = TypedDouble(tuple, tuple_desc, 2, sum_null);
          input.count = TypedInt(tuple, tuple_desc, 3);
          consumer(input);
        });
  }
  void ScanSub(
      const std::function<void(const query::SecEdgarQ3SubInput &)> &consumer)
      override {
    ScanTypedTuples(
        "SELECT adsh, name, cik, fy FROM sub",
        [&](HeapTuple tuple, TupleDesc tuple_desc) {
          query::SecEdgarQ3SubInput input;
          input.adsh = TypedText(tuple, tuple_desc, 0);
          input.name_is_null = IsNullAt(tuple, tuple_desc, 1);
          if (!input.name_is_null)
            input.name = TypedText(tuple, tuple_desc, 1);
          input.cik_is_null = IsNullAt(tuple, tuple_desc, 2);
          input.cik = input.cik_is_null
                          ? 0
                          : static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2));
          input.fy_is_null = IsNullAt(tuple, tuple_desc, 3);
          input.fy = input.fy_is_null
                         ? 0
                         : static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3));
          consumer(input);
        });
  }
};

Runtime<query::SecEdgarQ3Module, query::SecEdgarQ3Config, SecEdgarQ3PgScan> &
Q3Runtime() {
  static Runtime<query::SecEdgarQ3Module, query::SecEdgarQ3Config,
                 SecEdgarQ3PgScan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_SEC_EDGAR_Q3_COST_MODEL",
              "config/sec_edgar_q3_aux_cost_model.json",
              core::SecEdgarQ3Layout::kAccelerator,
              query::SecEdgarQ3Config{
                  kSecEdgarNum, kSecEdgarSub,
                  /* num attnums: adsh, uom, value */ {0, 5, 7},
                  /* sub attnums: adsh, name, cik, fy */ {0, 2, 1, 10},
                  /* positions in the projected rows */ {0, 1, 2},
                  {0, 1, 2, 3}});
  return runtime;
}

} // namespace
} // namespace detail

namespace {

void ExecutePostgresSecEdgarQ3(FunctionCallInfo fcinfo) {
  // accel_sec_edgar_q3_aux() uses the defaults; the two-argument overload is
  // not STRICT: a NULL argument reaches the module as NULL.
  core::QueryArguments arguments;
  if (PG_NARGS() >= 2) {
    arguments.values.push_back(
        PG_ARGISNULL(0) ? core::AuxValue::Null(core::AuxType::Int64)
                        : core::AuxValue::Of(core::AuxType::Int64,
                                             static_cast<int64_t>(PG_GETARG_INT64(0))));
    arguments.values.push_back(
        PG_ARGISNULL(1) ? core::AuxValue::Null(core::AuxType::String)
                        : core::AuxValue::Of(core::AuxType::String,
                                             detail::TextDatum(PG_GETARG_DATUM(1))));
  }
  detail::Q3Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "sec_edgar", 3, kPostgresVariantAux, &ExecutePostgresSecEdgarQ3});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
