// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q9 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q9/postgres/aux/module.hpp"
#include "tpch/q9/cost_model.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ9Parts(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ9PartInput &)> &sink) {
  ScanPartitioned(services, "part", {IntColumn("p_partkey"), TextColumn("p_name")}, partitions,
                  [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1)});
                    }
                  });
}

void ScanTpchQ9Suppliers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ9SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier", {IntColumn("s_suppkey"), IntColumn("s_nationkey")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      if (row.IsNull(0) || row.IsNull(1))
                        continue; // the contract returns joinable rows only
                      sink(partition, {row.Int(0), static_cast<int32_t>(row.Int(1))});
                    }
                  });
}

void ScanTpchQ9NationsSerial(
    const std::function<void(const query::TpchQ9NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Orders(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ9OrdersInput &)> &sink) {
  ScanPartitioned(services, "orders", {IntColumn("o_orderkey"), DateColumn("o_orderdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Date(1)});
                    }
                  });
}

void ScanTpchQ9Partsupps(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ9PartsuppInput &)> &sink) {
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

void ScanTpchQ9Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ9LineitemInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_partkey"), IntColumn("l_suppkey"), IntColumn("l_orderkey"),
                   DecimalColumn("l_quantity", 2), DecimalColumn("l_extendedprice", 2),
                   DecimalColumn("l_discount", 2)},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Int(2), row.Decimal(3),
                                       row.Decimal(4), row.Decimal(5)});
                    }
                  });
}

// Index lookup of the lineitems whose `column` (l_partkey, l_suppkey or
// l_orderkey) is one of `keys`, as the transaction leaves the database (this
// runs while staging maintenance at pre-commit).
void LookupTpchQ9Lineitems(
    const char *column, const std::vector<int64_t> &keys,
    const std::function<void(const query::TpchQ9LineitemInput &, int32_t)>
        &consumer) {
  constexpr std::size_t kBatch = 4096;
  for (std::size_t from = 0; from < keys.size(); from += kBatch) {
    const std::vector<int64_t> batch(
        keys.begin() + from,
        keys.begin() + std::min(keys.size(), from + kBatch));
    const std::string sql =
        std::string("SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, "
                    "l_extendedprice, l_discount, l_linenumber FROM lineitem "
                    "WHERE ") +
        KeyListPredicate(column, batch) +
        " AND l_partkey IS NOT NULL AND l_suppkey IS NOT NULL AND "
        "l_orderkey IS NOT NULL AND l_quantity IS NOT NULL AND "
        "l_extendedprice IS NOT NULL AND l_discount IS NOT NULL";
    ScanTypedTuples(
        sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
          consumer({TypedInt(tuple, tuple_desc, 0),
                    TypedInt(tuple, tuple_desc, 1),
                    TypedInt(tuple, tuple_desc, 2),
                    TypedDecimalRaw(tuple, tuple_desc, 3),
                    TypedDecimalRaw(tuple, tuple_desc, 4),
                    TypedDecimalRaw(tuple, tuple_desc, 5)},
                   static_cast<int32_t>(TypedInt(tuple, tuple_desc, 6)));
        });
  }
}

} // namespace scans

class TpchQ9Scan final : public query::TpchQ9InitialScan {
public:
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9PartInput &)> &sink)
      override {
    scans::ScanTpchQ9Parts(services, partitions, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ9Suppliers(services, partitions, sink);
  }
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ9NationInput &)> &consumer)
      override {
    scans::ScanTpchQ9NationsSerial(consumer);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9OrdersInput &)> &sink)
      override {
    scans::ScanTpchQ9Orders(services, partitions, sink);
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9PartsuppInput &)> &sink)
      override {
    scans::ScanTpchQ9Partsupps(services, partitions, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9LineitemInput &)> &sink)
      override {
    scans::ScanTpchQ9Lineitems(services, partitions, sink);
  }
  bool LookupLineitems(
      const char *column, const std::vector<int64_t> &keys,
      const std::function<void(const query::TpchQ9LineitemInput &, int32_t)>
          &consumer) override {
    scans::LookupTpchQ9Lineitems(column, keys, consumer);
    return true;
  }
};
Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan> &Q9Runtime() {
  static Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q9_COST_MODEL",
              core::kTpchQ9CostModel,
              query::TpchQ9Layout::kAccelerator,
              query::TpchQ9Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kPartsupp, pg_tbl::kLineitem},
              {{"part", pg_tbl::kPart}, {"supplier", pg_tbl::kSupplier}, {"nation", pg_tbl::kNation}, {"orders", pg_tbl::kOrders}, {"partsupp", pg_tbl::kPartsupp}, {"lineitem", pg_tbl::kLineitem}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ9(FunctionCallInfo fcinfo) {
  detail::Q9Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::String)
                                : core::AuxValue::Of(
                                      core::AuxType::String,
                                      detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 9, kPostgresVariantAux, &ExecutePostgresTpchQ9});
const detail::RuntimeRegistrar<&detail::Q9Runtime> kAcceleratorRegistrar(
    query::TpchQ9Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
