// MySQL plugin runtime for TPC-H Q21: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "tpch/mysql_table_ids.hpp"
#include "tpch/q21/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ21Suppliers(
    const std::function<void(const query::TpchQ21SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedInt(cols, 2)});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

// Lineitems of fulfilled orders, one order's rows consecutive. `restrict_sql`
// is an extra predicate (empty: every order).
void ScanTpchQ21FulfilledLineitems(
    const std::string &restrict_sql,
    const std::function<void(const query::TpchQ21LineInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, "
      "COALESCE(l_receiptdate > l_commitdate, 0) "
      "FROM lineitem JOIN orders ON o_orderkey = l_orderkey "
      "WHERE o_orderstatus = 'F'" +
          restrict_sql + " ORDER BY l_orderkey",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0), TypedInt(cols, 1),
                  TypedInt(cols, 2) != 0});
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
    scans::ScanTpchQ21FulfilledLineitems("", consumer);
  }
  bool LookupFulfilledLineitems(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ21LineInput &)> &consumer)
      override {
    if (orderkeys.empty())
      return true;
    std::string restrict_sql = " AND l_orderkey IN (";
    for (std::size_t i = 0; i < orderkeys.size(); ++i) {
      if (i)
        restrict_sql += ',';
      restrict_sql += std::to_string(orderkeys[i]);
    }
    restrict_sql += ')';
    scans::ScanTpchQ21FulfilledLineitems(restrict_sql, consumer);
    return true;
  }
};

Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan> &
Q21Runtime() {
  static Runtime<query::TpchQ21Module, query::TpchQ21Config, TpchQ21Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q21_COST_MODEL",
              "config/q21_aux_cost_model.json",
              query::TpchQ21Layout::kAccelerator,
              query::TpchQ21Config{mysql_tbl::kSupplier, mysql_tbl::kOrders, mysql_tbl::kLineitem, mysql_tbl::kNation});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q21Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ21Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q21_aux", "s", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
