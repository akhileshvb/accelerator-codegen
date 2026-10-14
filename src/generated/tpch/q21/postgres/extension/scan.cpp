// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q21 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q21/postgres/aux/module.hpp"
#include "tpch/q21/cost_model.hpp"



#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ21Suppliers(
    const std::function<void(const query::TpchQ21SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

// Lineitems of fulfilled orders, one order's rows consecutive. `restrict_sql`
// is an extra predicate (empty: every order).
const char *kFulfilledLineitemsSql =
    "SELECT l_orderkey, l_suppkey, "
    "COALESCE(l_receiptdate > l_commitdate, false)::int "
    "FROM lineitem JOIN orders ON o_orderkey = l_orderkey "
    "WHERE o_orderstatus = 'F'";

template <typename Scan>
void RunFulfilledLineitems(
    Scan &&scan, const std::string &restrict_sql,
    const std::function<void(const query::TpchQ21LineInput &)> &consumer) {
  scan(std::string(kFulfilledLineitemsSql) + restrict_sql +
           " ORDER BY l_orderkey",
       [&](HeapTuple tuple, TupleDesc tuple_desc) {
         consumer({TypedInt(tuple, tuple_desc, 0),
                   TypedInt(tuple, tuple_desc, 1),
                   TypedInt(tuple, tuple_desc, 2) != 0});
       });
}

} // namespace scans

class TpchQ21Scan final : public query::TpchQ21InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ21SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ21Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ21NationInput &)> &consumer)
      override {
    scans::ScanTpchQ21Nations(consumer);
  }
  void ScanFulfilledLineitems(
      const std::function<void(const query::TpchQ21LineInput &)> &consumer)
      override {
    scans::RunFulfilledLineitems(
        [](const std::string &sql, auto &&row) {
          ScanTypedTuples(sql, row);
        },
        "", consumer);
  }
  bool LookupFulfilledLineitems(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ21LineInput &)> &consumer)
      override {
    // Called from the capture trigger: read the database as the writing
    // statement (and this transaction) left it.
    scans::RunFulfilledLineitems(
        [](const std::string &sql, auto &&row) {
          ScanTypedTuplesAfterOwnWrites(sql, row);
        },
        // The key list on both join sides: PostgreSQL does not carry an
        // = ANY(...) across the join, and with it on lineitem only it hashes
        // every fulfilled order (seconds per lookup at SF20).
        " AND " + KeyListPredicate("l_orderkey", orderkeys) + " AND " +
            KeyListPredicate("o_orderkey", orderkeys),
        consumer);
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


} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
