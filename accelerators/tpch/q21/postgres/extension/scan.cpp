// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q21 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q21/postgres/aux/module.hpp"
#include "tpch/q21/cost_model.hpp"



#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ21Suppliers(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ21SupplierInput &)> &sink) {
  ScanPartitioned(services, "supplier",
                  {IntColumn("s_suppkey"), TextColumn("s_name"), IntColumn("s_nationkey")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Text(1), row.Int(2)});
                    }
                  });
}

void ScanTpchQ21NationsSerial(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21FulfilledOrders(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, int64_t)> &sink) {
  ScanPartitioned(services, "orders", {IntColumn("o_orderkey"), TextColumn("o_orderstatus")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      if (row.Char(1) == 'F')
                        sink(partition, row.Int(0));
                    }
                  });
}

void ScanTpchQ21Lineitems(
    const core::AuxEngineServices &services, std::size_t partitions,
    const std::function<void(std::size_t, const query::TpchQ21LineInput &)> &sink) {
  ScanPartitioned(services, "lineitem",
                  {IntColumn("l_orderkey"), IntColumn("l_suppkey"),
                   DateColumn("l_commitdate"), DateColumn("l_receiptdate")},
                  partitions, [&](std::size_t partition, const BuildBatch &batch) {
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                      const auto row = batch.Row(i);
                      sink(partition, {row.Int(0), row.Int(1), row.Date(3) > row.Date(2)});
                    }
                  });
}

// Lineitems of the given fulfilled orders, one order's rows consecutive.
void LookupTpchQ21FulfilledLineitems(
    const std::vector<int64_t> &orderkeys,
    const std::function<void(const query::TpchQ21LineInput &)> &consumer) {
  // The key list on both join sides: PostgreSQL does not carry an = ANY(...)
  // across the join, and with it on lineitem only it hashes every fulfilled
  // order (seconds per lookup at SF20).
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, "
      "COALESCE(l_receiptdate > l_commitdate, false)::int "
      "FROM lineitem JOIN orders ON o_orderkey = l_orderkey "
      "WHERE o_orderstatus = 'F' AND " +
          KeyListPredicate("l_orderkey", orderkeys) + " AND " +
          KeyListPredicate("o_orderkey", orderkeys) + " ORDER BY l_orderkey",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2) != 0});
      });
}

} // namespace scans

class TpchQ21Scan final : public query::TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ21SupplierInput &)> &sink)
      override {
    scans::ScanTpchQ21Suppliers(services, partitions, sink);
  }
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ21NationInput &)> &consumer)
      override {
    scans::ScanTpchQ21NationsSerial(consumer);
  }
  void ScanFulfilledOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, int64_t)> &sink) override {
    scans::ScanTpchQ21FulfilledOrders(services, partitions, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ21LineInput &)> &sink)
      override {
    scans::ScanTpchQ21Lineitems(services, partitions, sink);
  }
  bool LookupFulfilledLineitems(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ21LineInput &)> &consumer)
      override {
    // Called from the capture trigger: read the database as the writing
    // statement (and this transaction) left it.
    scans::LookupTpchQ21FulfilledLineitems(orderkeys, consumer);
    return true;
  }
};
Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              core::kTpchQ21CostModel,
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", pg_tbl::kSupplier}, {"orders", pg_tbl::kOrders}, {"lineitem", pg_tbl::kLineitem}, {"nation", pg_tbl::kNation}},
              {{"supplier", "s_name"}});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ21(FunctionCallInfo fcinfo) {
  // A NULL nation name stays NULL (n_name = NULL keeps no row).
  core::QueryArguments arguments;
  if (PG_ARGISNULL(0))
    arguments.values.push_back(core::AuxValue::Null(core::AuxType::String));
  else
    arguments.values.push_back(core::AuxValue::Of(
        core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0))));
  detail::Q21Runtime().Execute(arguments, fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 21, kPostgresVariantAux, &ExecutePostgresTpchQ21});
const detail::RuntimeRegistrar<&detail::Q21Runtime> kAcceleratorRegistrar(
    query::TpchQ21Layout::kAccelerator);


} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
