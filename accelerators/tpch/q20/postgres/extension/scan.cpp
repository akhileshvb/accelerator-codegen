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
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ20LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {DateColumn("l_shipdate"), IntColumn("l_partkey"),
                   IntColumn("l_suppkey"), DecimalColumn("l_quantity", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Date(0), row.Int(1), row.Int(2), row.Decimal(3)});
                    }
                  });
}

void ScanTpchQ20Parts(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ20PartInput &)> &sink) {
  ScanPartitioned(services, "part", {IntColumn("p_partkey"), TextColumn("p_name")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1)});
                    }
                  });
}

void ScanTpchQ20Partsupps(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ20PartsuppInput &)> &sink) {
  ScanPartitioned(services, "partsupp",
                  {IntColumn("ps_partkey"), IntColumn("ps_suppkey"), IntColumn("ps_availqty")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Int(2)});
                    }
                  });
}

void ScanTpchQ20Suppliers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ20SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier",
                  {Nullable(IntColumn("s_suppkey")), TextColumn("s_name"),
                   TextColumn("s_address"), Nullable(IntColumn("s_nationkey"))},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      if (row.IsNull(0) || row.IsNull(3))
                        continue;
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2),
                                       static_cast<int32_t>(row.Int(3))});
                    }
                  });
}

void ScanTpchQ20NationsSerial(
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
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ20Lineitems(services, partitions, sink);
  }
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20PartInput &)> &sink)
      override {
    scans::ScanTpchQ20Parts(services, partitions, sink);
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20PartsuppInput &)> &sink)
      override {
    scans::ScanTpchQ20Partsupps(services, partitions, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ20Suppliers(services, partitions, sink);
  }
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ20NationInput &)> &consumer)
      override {
    scans::ScanTpchQ20NationsSerial(consumer);
  }
};
Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan> &
Q20Runtime() {
  static Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q20_COST_MODEL",
              core::kTpchQ20CostModel,
              query::TpchQ20Layout::kAccelerator,
              query::TpchQ20Config{pg_tbl::kLineitem, pg_tbl::kPart, pg_tbl::kPartsupp, pg_tbl::kSupplier, pg_tbl::kNation},
              {{"lineitem", pg_tbl::kLineitem}, {"part", pg_tbl::kPart}, {"partsupp", pg_tbl::kPartsupp}, {"supplier", pg_tbl::kSupplier}, {"nation", pg_tbl::kNation}});
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
const detail::RuntimeRegistrar<&detail::Q20Runtime> kAcceleratorRegistrar(
    query::TpchQ20Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
