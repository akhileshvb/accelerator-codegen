// GENERATED BACKEND — PostgreSQL scans + runtime for TPC-H Q21 (module only).
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

// Finalize headers pull in DuckDB types; include them before PostgreSQL
// headers to avoid timestamp.h macro collisions.
#include "tpch/q21/postgres/aux/module.hpp"

#if defined(ACCEL_HAVE_Q21_VERUS)
#include "tpch/q21/verus/module.hpp"
#endif


#include "accelerator_codegen/stateful/postgres/postgres_tpch_table_ids.hpp"
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
                  static_cast<int32_t>(TypedInt(tuple, tuple_desc, 2))});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({static_cast<int32_t>(TypedInt(tuple, tuple_desc, 0)),
                  TypedText(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Orders(
    const std::function<void(const query::TpchQ21OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedChar(tuple, tuple_desc, 1)});
      });
}

void ScanTpchQ21Lineitems(
    const std::function<void(const query::TpchQ21LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
      "FROM lineitem l "
      "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
      "o.o_orderstatus = 'F'",
      [&](HeapTuple tuple, TupleDesc tuple_desc) {
        consumer({TypedInt(tuple, tuple_desc, 0),
                  TypedInt(tuple, tuple_desc, 1),
                  TypedDate(tuple, tuple_desc, 2),
                  TypedDate(tuple, tuple_desc, 3)});
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
  void ScanOrders(
      const std::function<void(const query::TpchQ21OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ21Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ21LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ21Lineitems(consumer);
  }
};
#if defined(ACCEL_HAVE_Q21_VERUS)
// Scans for the Verus Q21 kernel.  Each implements the SQL its interface
// comment names (the kernel's trusted inputs).
class TpchQ21VerusScanImpl final : public query::TpchQ21VerusScan {
public:
  void ScanSuppliers(
      const std::function<void(int64_t, const std::string &, int32_t)> &consumer)
      override {
    ScanTypedTuples("SELECT s_suppkey, s_name, s_nationkey FROM supplier",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(TypedInt(tuple, desc, 0), TypedCharText(tuple, desc, 1),
                               static_cast<int32_t>(TypedInt(tuple, desc, 2)));
                    });
  }
  void ScanNations(
      const std::function<void(int32_t, const std::string &)> &consumer) override {
    ScanTypedTuples("SELECT n_nationkey, n_name FROM nation",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(static_cast<int32_t>(TypedInt(tuple, desc, 0)),
                               TypedCharText(tuple, desc, 1));
                    });
  }
  void NormalizeCapturedName(bool supplier, std::string &name) const override {
    NormalizeCapturedText(supplier ? "supplier" : "nation",
                          supplier ? "s_name" : "n_name", name);
  }
  void ScanFOrderKeys(const std::function<void(int64_t)> &consumer) override {
    ScanTypedTuples("SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F'",
                    [&](HeapTuple tuple, TupleDesc desc) {
                      consumer(TypedInt(tuple, desc, 0));
                    });
  }
  void ScanFLineitems(const std::function<void(const q21v_li_row &)> &consumer) override {
    ScanTypedTuples(
        "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
        "FROM lineitem l WHERE EXISTS (SELECT 1 FROM orders o "
        "WHERE o.o_orderkey = l.l_orderkey AND o.o_orderstatus = 'F')",
        [&](HeapTuple tuple, TupleDesc desc) {
          consumer(q21v_li_row{TypedInt(tuple, desc, 0), TypedInt(tuple, desc, 1),
                               TypedDate(tuple, desc, 2), TypedDate(tuple, desc, 3)});
        });
  }
  void ScanLineitemsForKeys(const std::vector<int64_t> &keys,
                            const std::function<void(const q21v_li_row &)> &consumer)
      override {
    const std::string sql =
        "SELECT l_orderkey, l_suppkey, l_commitdate, l_receiptdate FROM lineitem "
        "WHERE " + KeyListPredicate("l_orderkey", keys);
    // Runs in PRE_COMMIT: the database after this transaction (the sync
    // protocol rules out other writers for a store that stays synced).
    ScanTypedTuplesAfterOwnWrites(sql, [&](HeapTuple tuple, TupleDesc desc) {
      consumer(q21v_li_row{TypedInt(tuple, desc, 0), TypedInt(tuple, desc, 1),
                           TypedDate(tuple, desc, 2), TypedDate(tuple, desc, 3)});
    });
  }
  void RunReference(const std::string &nation,
                    const std::function<void(const std::string &, int64_t)> &consumer)
      override {
    static const char *kSql =
        "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
        "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
        "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
        "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
        "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
        "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
        "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
        "n_name = $1 GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100";
    if (SPI_connect() != SPI_OK_CONNECT)
      throw std::runtime_error("SPI_connect failed");
    Oid argtypes[1] = {TEXTOID};
    Datum values[1] = {CStringGetTextDatum(nation.c_str())};
    ++t_table_scans;
    if (SPI_execute_with_args(kSql, 1, argtypes, values, nullptr, true, 0) !=
        SPI_OK_SELECT) {
      SPI_finish();
      throw std::runtime_error("Q21 reference query failed");
    }
    std::vector<std::pair<std::string, int64_t>> rows;
    for (uint64 i = 0; i < SPI_processed; ++i) {
      HeapTuple tuple = SPI_tuptable->vals[i];
      TupleDesc desc = SPI_tuptable->tupdesc;
      rows.emplace_back(TypedCharText(tuple, desc, 0), TypedInt(tuple, desc, 1));
    }
    SPI_finish();
    for (const auto &[name, numwait] : rows)
      consumer(name, numwait);
  }
};
#endif
Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{pg_tbl::kSupplier, pg_tbl::kOrders, pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", "s_name"}});
  return runtime;
}
#if defined(ACCEL_HAVE_Q21_VERUS)
Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig, TpchQ21VerusScanImpl> &
Q21VerusRuntime() {
  static Runtime<query::TpchQ21VerusModule, query::TpchQ21VerusConfig,
                 TpchQ21VerusScanImpl>
      runtime("ACCELERATOR_CODEGEN_POSTGRES_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21VerusLayout::kAccelerator,
              query::TpchQ21VerusConfig{pg_tbl::kSupplier, pg_tbl::kOrders,
                                        pg_tbl::kLineitem, pg_tbl::kNation},
              {{"supplier", "s_name"}});
  return runtime;
}
#endif
} // namespace
} // namespace detail

namespace core = accelerator_codegen::aux_state::core;

namespace {

void ExecutePostgresTpchQ21(FunctionCallInfo fcinfo) {
  detail::Q21Runtime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}
#if defined(ACCEL_HAVE_Q21_VERUS)
void ExecutePostgresTpchQ21Verus(FunctionCallInfo fcinfo) {
  detail::Q21VerusRuntime().Execute(
      core::QueryArguments{{core::AuxValue::Of(
          core::AuxType::String, detail::TextDatum(PG_GETARG_DATUM(0)))}},
      fcinfo);
}
#endif

const PostgresQueryExecutorRegistrar kAuxRegistrar(PostgresQueryExecutor{
    "tpch", 21, kPostgresVariantAux, &ExecutePostgresTpchQ21});

#if defined(ACCEL_HAVE_Q21_VERUS)
const PostgresQueryExecutorRegistrar kVerusRegistrar(PostgresQueryExecutor{
    "tpch", 21, kPostgresVariantAuxVerus, &ExecutePostgresTpchQ21Verus});
#endif

} // namespace
} // namespace accelerator_codegen::aux_state::engine::postgres

#endif
