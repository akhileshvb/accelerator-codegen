// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q2 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q2/postgres/aux/module.hpp"
#include "tpch/q2/cost_model.hpp"
#include "tpch/q2/layout.hpp"


#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ2Parts(
    const std::function<void(const query::TpchQ2PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_mfgr, p_type, p_size FROM part",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ2Suppliers(
    const std::function<void(const query::TpchQ2SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, "
      "s_comment FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  TypedInt(tuple, tuple_desc, 3),
                  TypedText(tuple, tuple_desc, 4),
                  TypedDecimalRaw(tuple, tuple_desc, 5),
                  TypedText(tuple, tuple_desc, 6)});
      });
}

void ScanTpchQ2Partsupps(
    const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ2Nations(
    const std::function<void(const query::TpchQ2NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ2Regions(
    const std::function<void(const query::TpchQ2RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ2Scan final : public query::TpchQ2InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ2PartInput &)> &consumer)
      override {
    scans::ScanTpchQ2Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ2SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ2Suppliers(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ2Partsupps(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ2NationInput &)> &consumer)
      override {
    scans::ScanTpchQ2Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ2RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ2Regions(consumer);
  }
};
Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan> &Q2Runtime() {
  static Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q2_COST_MODEL",
              core::kTpchQ2CostModel, core::TpchQ2Layout::kAccelerator,
              query::TpchQ2Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kPartsupp, pg_tbl::kNation, pg_tbl::kRegion});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ2(FunctionCallInfo fcinfo) {
  // The SQL function is not STRICT: a NULL argument reaches the module as a
  // NULL AuxValue (the predicate on it is then never true).
  const auto text_arg = [&](int index) {
    return PG_ARGISNULL(index)
               ? core::AuxValue::Null(core::AuxType::String)
               : core::AuxValue::Of(core::AuxType::String,
                                    detail::TextDatum(PG_GETARG_DATUM(index)));
  };
  detail::Q2Runtime().Execute(
      core::QueryArguments{
          {PG_ARGISNULL(0)
               ? core::AuxValue::Null(core::AuxType::Int64)
               : core::AuxValue::Of(core::AuxType::Int64,
                                    static_cast<int64_t>(PG_GETARG_INT32(0))),
           text_arg(1), text_arg(2)}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 2, kPostgresVariantAux, &ExecutePostgresTpchQ2});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
