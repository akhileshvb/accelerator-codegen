// MySQL plugin runtime for TPC-H Q21: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
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
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ21Nations(
    const std::function<void(const query::TpchQ21NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ21Orders(
    const std::function<void(const query::TpchQ21OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderstatus FROM orders WHERE o_orderstatus = 'F'",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedChar(cols, 1)});
      });
}

void ScanTpchQ21Lineitems(
    const std::function<void(const query::TpchQ21LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l.l_orderkey, l.l_suppkey, l.l_commitdate, l.l_receiptdate "
      "FROM lineitem l "
      "INNER JOIN orders o ON l.l_orderkey = o.o_orderkey AND "
      "o.o_orderstatus = 'F'",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2),
                  TypedDate(cols, 3)});
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
