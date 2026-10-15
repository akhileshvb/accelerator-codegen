// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q12 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q12/postgres/aux/module.hpp"
#include "tpch/q12/cost_model.hpp"
#include "tpch/q12/layout.hpp"


#include "tpch/common/postgres_tpch_table_ids.hpp"
#include "backends/postgres/api/engine_runtime.hpp"
#include "backends/postgres/api/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanColumns("orders",
              {IntColumn("o_orderkey"), TextColumn("o_orderpriority")},
              [&](const BuildRow &row) {
                consumer({row.Int(0), row.Text(1), false});
              });
}

// Lineitems that can contribute: non-NULL key / shipmode, commit < receipt,
// ship < commit (NULL dates fail the comparisons).  `where` narrows further.
void ScanTpchQ12QualifyingLineitems(
    const std::string &where,
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate "
      "FROM lineitem WHERE l_orderkey IS NOT NULL AND l_shipmode IS NOT NULL "
      "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate" +
          where,
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3),
                  TypedDate(tuple, tuple_desc, 4)});
      });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTpchQ12QualifyingLineitems("", consumer);
}

// Index lookup of the qualifying lineitems of some orders, as the transaction
// leaves the database (this runs while staging maintenance at pre-commit).
void LookupTpchQ12LineitemsByOrder(
    const std::vector<int64_t> &orderkeys,
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  constexpr std::size_t kBatch = 4096;
  for (std::size_t from = 0; from < orderkeys.size(); from += kBatch) {
    const std::vector<int64_t> batch(
        orderkeys.begin() + from,
        orderkeys.begin() + std::min(orderkeys.size(), from + kBatch));
    const std::string sql =
        std::string("SELECT l_orderkey, l_shipmode, l_commitdate, "
                    "l_receiptdate, l_shipdate FROM lineitem WHERE ") +
        KeyListPredicate("l_orderkey", batch) +
        " AND l_shipmode IS NOT NULL AND l_commitdate < l_receiptdate "
        "AND l_shipdate < l_commitdate";
    ScanTypedTuples(
        sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
          consumer({TypedInt(tuple, tuple_desc, 0),
                    TypedText(tuple, tuple_desc, 1),
                    TypedDate(tuple, tuple_desc, 2),
                    TypedDate(tuple, tuple_desc, 3),
                    TypedDate(tuple, tuple_desc, 4)});
        });
  }
}

} // namespace scans

class TpchQ12Scan final : public query::TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ12OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ12Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ12Lineitems(consumer);
  }
  void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ12LineitemsByOrder(orderkeys, consumer);
  }
};
Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q12_COST_MODEL",
              core::kTpchQ12CostModel,
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

// SQL NULL arguments reach the module as NULL values.
void ExecutePostgresTpchQ12(FunctionCallInfo fcinfo) {
  detail::Q12Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::String)
                                : core::AuxValue::Of(
                                      core::AuxType::String,
                                      detail::TextDatum(PG_GETARG_DATUM(0))),
                            PG_ARGISNULL(1)
                                ? core::AuxValue::Null(core::AuxType::String)
                                : core::AuxValue::Of(
                                      core::AuxType::String,
                                      detail::TextDatum(PG_GETARG_DATUM(1))),
                            PG_ARGISNULL(2)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(2, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 12, kPostgresVariantAux, &ExecutePostgresTpchQ12});
const detail::RuntimeRegistrar<&detail::Q12Runtime> kAcceleratorRegistrar(
    core::TpchQ12Layout::kAccelerator);

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
