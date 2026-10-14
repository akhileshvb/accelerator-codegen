// MySQL plugin runtime for TPC-H Q9: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "tpch/mysql_table_ids.hpp"
#include "tpch/q9/mysql/aux/module.hpp"
#include "tpch/q9/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ9Parts(
    const std::function<void(const query::TpchQ9PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part "
      "WHERE p_partkey IS NOT NULL AND p_name IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ9Suppliers(
    const std::function<void(const query::TpchQ9SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ9Nations(
    const std::function<void(const query::TpchQ9NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ9Orders(
    const std::function<void(const query::TpchQ9OrdersInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate FROM orders "
      "WHERE o_orderkey IS NOT NULL AND o_orderdate IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1)});
      });
}

void ScanTpchQ9Partsupps(
    const std::function<void(const query::TpchQ9PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp "
      "WHERE ps_partkey IS NOT NULL AND ps_suppkey IS NOT NULL AND "
      "ps_supplycost IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2)});
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
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3),
                  TypedDecimalRaw(cols, 4),
                  TypedDecimalRaw(cols, 5)});
      });
}


// Pre-aggregation in SQL: the DBMS runs the join lineitem x partsupp x supplier
// x orders and groups it by (part, supplier nation, order year); the module
// keeps those groups as its profit cells. Amounts are in 1e-4 units.
void ScanTpchQ9ProfitContributions(
    const std::function<void(const query::TpchQ9ProfitContribution &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, s_nationkey, YEAR(o_orderdate), COUNT(*), "
      "CAST(SUM(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) "
      "* 10000 AS SIGNED) "
      "FROM lineitem, partsupp, supplier, orders "
      "WHERE ps_suppkey = l_suppkey AND ps_partkey = l_partkey "
      "AND s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
      "AND l_extendedprice IS NOT NULL AND l_discount IS NOT NULL "
      "AND l_quantity IS NOT NULL AND ps_supplycost IS NOT NULL "
      "GROUP BY l_partkey, s_nationkey, YEAR(o_orderdate)",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1)),
                  static_cast<int32_t>(TypedInt(cols, 2)),
                  TypedInt(cols, 3),
                  TypedInt(cols, 4)});
      });
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
};

Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan> &Q9Runtime() {
  static Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q9_COST_MODEL",
              core::kTpchQ9CostModel,
              query::TpchQ9Layout::kAccelerator,
              query::TpchQ9Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kNation, mysql_tbl::kOrders, mysql_tbl::kPartsupp, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q9Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ9Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q9_aux", "s", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
