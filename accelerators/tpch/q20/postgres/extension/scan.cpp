// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q20 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q20/postgres/aux/module.hpp"
#include "tpch/q20/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ20Lineitems(
    const std::function<void(const query::TpchQ20LineitemInput &)> &consumer) {
  ScanColumns("lineitem",
              {DateColumn("l_shipdate"), IntColumn("l_partkey"),
               IntColumn("l_suppkey"), DecimalColumn("l_quantity", 2)},
              [&](const BuildRow &row) {
                consumer({row.Date(0),
                          row.Int(1),
                          row.Int(2),
                          row.Decimal(3)});
              });
}

void ScanTpchQ20Parts(
    const std::function<void(const query::TpchQ20PartInput &)> &consumer) {
  ScanColumns("part",
              {IntColumn("p_partkey"), TextColumn("p_name")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Text(1)});
              });
}

void ScanTpchQ20Partsupps(
    const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer) {
  ScanColumns("partsupp",
              {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
               IntColumn("ps_availqty")},
              [&](const BuildRow &row) {
                consumer({row.Int(0),
                          row.Int(1),
                          row.Int(2)});
              });
}

void ScanTpchQ20Suppliers(
    const std::function<void(const query::TpchQ20SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedText(tuple, tuple_desc, 2),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 3))});
      });
}

void ScanTpchQ20Nations(
    const std::function<void(const query::TpchQ20NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

} // namespace scans

class TpchQ20Scan final : public query::TpchQ20InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ20LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ20Lineitems(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ20PartInput &)> &consumer)
      override {
    scans::ScanTpchQ20Parts(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ20Partsupps(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ20SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ20Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ20NationInput &)> &consumer)
      override {
    scans::ScanTpchQ20Nations(consumer);
  }
};
Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan> &
Q20Runtime() {
  static Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q20_COST_MODEL",
              core::kTpchQ20CostModel,
              query::TpchQ20Layout::kAccelerator,
              query::TpchQ20Config{pg_tbl::kLineitem, pg_tbl::kPart, pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ20(FunctionCallInfo fcinfo) {
  detail::Q20Runtime().Execute(
      core::QueryArguments{
          {PG_ARGISNULL(0) ? core::AuxValue::Null(core::AuxType::Date)
                           : core::AuxValue::Of(core::AuxType::Date,
                                                detail::PostgresDateArg(0, fcinfo)),
           PG_ARGISNULL(1) ? core::AuxValue::Null(core::AuxType::String)
                           : core::AuxValue::Of(core::AuxType::String,
                                                detail::TextDatum(PG_GETARG_DATUM(1))),
           PG_ARGISNULL(2) ? core::AuxValue::Null(core::AuxType::String)
                           : core::AuxValue::Of(core::AuxType::String,
                                                detail::TextDatum(PG_GETARG_DATUM(2)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 20, kPostgresVariantAux, &ExecutePostgresTpchQ20});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
