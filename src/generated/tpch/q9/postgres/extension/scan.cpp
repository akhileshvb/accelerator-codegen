// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q9 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q9/postgres/aux/module.hpp"
#include "tpch/q9/cost_model.hpp"


#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ9Parts(
    const std::function<void(const query::TpchQ9PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part "
      "WHERE p_partkey IS NOT NULL AND p_name IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Suppliers(
    const std::function<void(const query::TpchQ9SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ9Nations(
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
    const std::function<void(const query::TpchQ9OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate FROM orders "
      "WHERE o_orderkey IS NOT NULL AND o_orderdate IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedDate(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ9Partsupps(
    const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp "
      "WHERE ps_partkey IS NOT NULL AND ps_suppkey IS NOT NULL AND "
      "ps_supplycost IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDecimalRaw(tuple, tuple_desc, 2)});
      });
}

void ScanTpchQ9Lineitems(
    const std::function<void(const query::TpchQ9LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, l_extendedprice, "
      "l_discount FROM lineitem "
      "WHERE l_partkey IS NOT NULL AND l_suppkey IS NOT NULL AND "
      "l_orderkey IS NOT NULL AND l_quantity IS NOT NULL AND "
      "l_extendedprice IS NOT NULL AND l_discount IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2),
                  TypedDecimalRaw(tuple, tuple_desc, 3),
                  TypedDecimalRaw(tuple, tuple_desc, 4),
                  TypedDecimalRaw(tuple, tuple_desc, 5)});
      });
}


// Pre-aggregation in SQL: the DBMS runs the join lineitem x partsupp x supplier
// x orders and groups it by (part, supplier nation, order year); the module
// keeps those groups as its profit cells. Amounts are in 1e-4 units.
void ScanTpchQ9ProfitContributions(
    const std::function<void(const query::TpchQ9ProfitContribution &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, s_nationkey, EXTRACT(year FROM o_orderdate)::int4, "
      "COUNT(*), "
      "(SUM(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) "
      "* 10000)::int8 "
      "FROM lineitem, partsupp, supplier, orders "
      "WHERE ps_suppkey = l_suppkey AND ps_partkey = l_partkey "
      "AND s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
      "AND l_extendedprice IS NOT NULL AND l_discount IS NOT NULL "
      "AND l_quantity IS NOT NULL AND ps_supplycost IS NOT NULL "
      "GROUP BY l_partkey, s_nationkey, 3",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1)),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2)),
                  TypedInt(tuple, tuple_desc, 3),
                  TypedInt(tuple, tuple_desc, 4)});
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
    ScanTypedTuplesAfterOwnWrites(
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
      const std::function<void(const query::TpchQ9PartInput &)> &consumer)
      override {
    scans::ScanTpchQ9Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ9SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ9Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ9NationInput &)> &consumer)
      override {
    scans::ScanTpchQ9Nations(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ9OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ9Orders(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ9Partsupps(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ9LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ9Lineitems(consumer);
  }
  bool ScanProfitContributions(
      const std::function<void(const query::TpchQ9ProfitContribution &)> &consumer)
      override {
    scans::ScanTpchQ9ProfitContributions(consumer);
    return true;
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
              query::TpchQ9Config{pg_tbl::kPart, pg_tbl::kSupplier, pg_tbl::kNation, pg_tbl::kOrders, pg_tbl::kPartsupp, pg_tbl::kLineitem});
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

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
