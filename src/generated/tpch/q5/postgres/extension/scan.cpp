// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q5 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q5/postgres/aux/module.hpp"
#include "tpch/q5/layout.hpp"


#include "tpch/postgres_tpch_table_ids.hpp"
#include "postgres/engine_runtime.hpp"
#include "postgres/runtime.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {
namespace detail {
namespace {

namespace pg_tbl = accelerator_codegen::aux_state::engine::postgres::tpch_tables;

namespace scans {

void ScanTpchQ5Regions(
    const std::function<void(const query::TpchQ5RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region "
      "WHERE r_regionkey IS NOT NULL AND r_name IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ5Nations(
    const std::function<void(const query::TpchQ5NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey, (n_name IS NULL)::int, "
      "(n_regionkey IS NULL)::int FROM nation WHERE n_nationkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2)),
                  TypedInt(tuple, tuple_desc, 3) != 0,
                  TypedInt(tuple, tuple_desc, 4) != 0});
      });
}

void ScanTpchQ5Customers(
    const std::function<void(const query::TpchQ5CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer "
      "WHERE c_custkey IS NOT NULL AND c_nationkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

void ScanTpchQ5Suppliers(
    const std::function<void(const query::TpchQ5SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 1))});
      });
}

// Orders that can join: NULL custkey / orderdate never reach the revenue sum.
const char kOrdersSql[] =
    "SELECT o_orderkey, o_custkey, o_orderdate FROM orders "
    "WHERE o_orderkey IS NOT NULL AND o_custkey IS NOT NULL "
    "AND o_orderdate IS NOT NULL";

void ScanTpchQ5Orders(
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  ScanTypedTuples(
      kOrdersSql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2)});
      });
}

// Lineitems that reach the SUM; price and discount as exact cents.
const char kLineitemsSql[] =
    "SELECT l_orderkey, l_suppkey, (l_extendedprice * 100)::bigint, "
    "(l_discount * 100)::bigint FROM lineitem "
    "WHERE l_orderkey IS NOT NULL AND l_suppkey IS NOT NULL "
    "AND l_extendedprice IS NOT NULL AND l_discount IS NOT NULL";

void ScanTpchQ5Lineitems(
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  ScanTypedTuples(
      kLineitemsSql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedInt(tuple, tuple_desc, 2), 2,
                  TypedInt(tuple, tuple_desc, 3), 2});
      });
}

// Index lookups while maintenance is staged at pre-commit: they read the
// database as this transaction leaves it.  Keys go in batches.
template <typename Consumer>
void LookupBatches(const std::string &base_sql, const char *key_column,
                   const std::vector<int64_t> &keys, Consumer &&scan_batch) {
  constexpr std::size_t kBatch = 4096;
  for (std::size_t from = 0; from < keys.size(); from += kBatch) {
    const std::vector<int64_t> batch(
        keys.begin() + from,
        keys.begin() + std::min(keys.size(), from + kBatch));
    scan_batch(base_sql + " AND " + KeyListPredicate(key_column, batch));
  }
}

void LookupTpchQ5Lineitems(
    const char *key_column, const std::vector<int64_t> &keys,
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  LookupBatches(kLineitemsSql, key_column, keys, [&](const std::string &sql) {
    ScanTypedTuplesAfterOwnWrites(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
      consumer({TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
                TypedInt(tuple, tuple_desc, 2), 2,
                TypedInt(tuple, tuple_desc, 3), 2});
    });
  });
}

void LookupTpchQ5OrdersByCustomer(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  LookupBatches(kOrdersSql, "o_custkey", custkeys, [&](const std::string &sql) {
    ScanTypedTuplesAfterOwnWrites(sql, [&](HeapTuple tuple, TupleDesc tuple_desc) {
      consumer({TypedInt(tuple, tuple_desc, 0), TypedInt(tuple, tuple_desc, 1),
                TypedDate(tuple, tuple_desc, 2)});
    });
  });
}

} // namespace scans

class TpchQ5Scan final : public query::TpchQ5InitialScan {
public:
  void ScanRegions(
      const std::function<void(const query::TpchQ5RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ5Regions(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ5NationInput &)> &consumer)
      override {
    scans::ScanTpchQ5Nations(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ5CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ5Customers(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ5SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ5Suppliers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ5Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ5Lineitems(consumer);
  }
  void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ5Lineitems("l_orderkey", orderkeys, consumer);
  }
  void LookupLineitemsBySupplier(
      const std::vector<int64_t> &suppkeys,
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ5Lineitems("l_suppkey", suppkeys, consumer);
  }
  void LookupOrdersByCustomer(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::LookupTpchQ5OrdersByCustomer(custkeys, consumer);
  }
};
Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan> &Q5Runtime() {
  static Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q5_COST_MODEL",
              "config/q5_aux_cost_model.json", core::TpchQ5Layout::kAccelerator,
              query::TpchQ5Config{pg_tbl::kRegion, pg_tbl::kNation, pg_tbl::kCustomer, pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem});
  return runtime;
}
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

// SQL NULL arguments reach the module as NULL values.
void ExecutePostgresTpchQ5(FunctionCallInfo fcinfo) {
  detail::Q5Runtime().Execute(
      core::QueryArguments{{PG_ARGISNULL(0)
                                ? core::AuxValue::Null(core::AuxType::String)
                                : core::AuxValue::Of(
                                      core::AuxType::String,
                                      detail::TextDatum(PG_GETARG_DATUM(0))),
                            PG_ARGISNULL(1)
                                ? core::AuxValue::Null(core::AuxType::Date)
                                : core::AuxValue::Of(
                                      core::AuxType::Date,
                                      detail::PostgresDateArg(1, fcinfo))}},
      fcinfo);
}

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 5, kPostgresVariantAux, &ExecutePostgresTpchQ5});

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
