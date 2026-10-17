// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q16 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q16/postgres/aux/module.hpp"
#include "tpch/q16/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ16Suppliers(
    const std::function<void(const query::TpchQ16SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_comment FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ16Parts(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ16PartInput &)> &sink) {
  ScanPartitioned(services, "part",
                  {IntColumn("p_partkey"), TextColumn("p_brand"),
                   TextColumn("p_type"), IntColumn("p_size")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Text(2),
                                       static_cast<int32_t>(row.Int(3))});
                    }
                  });
}

void ScanTpchQ16Partsupp(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ16PartsuppInput &)> &sink) {
  ScanPartitioned(services, "partsupp",
                  {IntColumn("ps_partkey"), IntColumn("ps_suppkey")}, partitions,
                  [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1)});
                    }
                  });
}

} // namespace scans

class TpchQ16Scan final : public query::TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ16SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ16Suppliers(consumer);
  }
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ16PartInput &)> &sink)
      override {
    scans::ScanTpchQ16Parts(services, partitions, sink);
  }
  void ScanPartsupp(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ16PartsuppInput &)> &sink)
      override {
    scans::ScanTpchQ16Partsupp(services, partitions, sink);
  }
};
Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan> &
Q16Runtime() {
  static Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q16_COST_MODEL",
              core::kTpchQ16CostModel,
              query::TpchQ16Layout::kAccelerator,
              query::TpchQ16Config{pg_tbl::kPart, pg_tbl::kPartsupp, pg_tbl::kSupplier},
              {{"part", pg_tbl::kPart}, {"partsupp", pg_tbl::kPartsupp}, {"supplier", pg_tbl::kSupplier}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ16(FunctionCallInfo fcinfo) {
  // The SQL function is not STRICT: a NULL argument reaches the module as a
  // NULL AuxValue (brand / pattern: the predicate is never true; a size item
  // simply drops out of the IN list).
  const auto text_arg = [&](int index) {
    return PG_ARGISNULL(index)
               ? core::AuxValue::Null(core::AuxType::String)
               : core::AuxValue::Of(core::AuxType::String,
                                    detail::TextDatum(PG_GETARG_DATUM(index)));
  };
  core::QueryArguments arguments;
  arguments.values.push_back(text_arg(0));
  arguments.values.push_back(text_arg(1));
  for (int index = 2; index < 10; ++index)
    arguments.values.push_back(
        PG_ARGISNULL(index)
            ? core::AuxValue::Null(core::AuxType::Int64)
            : core::AuxValue::Of(core::AuxType::Int64,
                                 static_cast<int64_t>(PG_GETARG_INT32(index))));
  detail::Q16Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 16, kPostgresVariantAux, &ExecutePostgresTpchQ16});
const detail::RuntimeRegistrar<&detail::Q16Runtime> kAcceleratorRegistrar(
    query::TpchQ16Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
