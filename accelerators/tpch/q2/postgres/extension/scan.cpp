// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q2 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q2/postgres/aux/module.hpp"
#include "tpch/q2/cost_model.hpp"
#include "tpch/q2/layout.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ2PartsPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ2PartInput &)> &sink) {
  ScanPartitioned(services, "part",
                  {IntColumn("p_partkey"), TextColumn("p_mfgr"),
                   TextColumn("p_type"), IntColumn("p_size")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2),
                                       static_cast<int32_t>(row.Int(3))});
                    }
                  });
}

void ScanTpchQ2SuppliersPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ2SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier",
                  {IntColumn("s_suppkey"), TextColumn("s_name"),
                   TextColumn("s_address"), IntColumn("s_nationkey"),
                   TextColumn("s_phone"), DecimalColumn("s_acctbal", 2),
                   TextColumn("s_comment")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2), row.Int(3),
                                       row.Text(4), row.Decimal(5), row.Text(6)});
                    }
                  });
}

void ScanTpchQ2PartsuppsPartitioned(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ2PartsuppInput &)> &sink) {
  ScanPartitioned(services, "partsupp",
                  {IntColumn("ps_partkey"), IntColumn("ps_suppkey"),
                   DecimalColumn("ps_supplycost", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Decimal(2)});
                    }
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
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2PartInput &)> &sink)
      override {
    scans::ScanTpchQ2PartsPartitioned(services, partitions, sink);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ2SuppliersPartitioned(services, partitions, sink);
  }
  void ScanPartsuppsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2PartsuppInput &)> &sink)
      override {
    scans::ScanTpchQ2PartsuppsPartitioned(services, partitions, sink);
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
              query::TpchQ2Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kPartsupp, pg_tbl::kNation, pg_tbl::kRegion},
              {{"part", pg_tbl::kPart}, {"supplier", pg_tbl::kSupplier}, {"partsupp", pg_tbl::kPartsupp}, {"nation", pg_tbl::kNation}, {"region", pg_tbl::kRegion}});
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
const detail::RuntimeRegistrar<&detail::Q2Runtime> kAcceleratorRegistrar(
    core::TpchQ2Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
