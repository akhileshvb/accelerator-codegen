// MySQL plugin runtime for TPC-H Q3: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q3/mysql/aux/module.hpp"
#include "tpch/q3/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ3Customers(
    const std::function<void(const query::TpchQ3CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_mktsegment FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ3Orders(
    const std::function<void(const query::TpchQ3OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderdate, o_custkey, o_shippriority FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDate(cols, 1),
                  TypedInt(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ3Lineitems(
    const std::function<void(const query::TpchQ3LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDate(cols, 3)});
      });
}

} // namespace scans

class TpchQ3Scan final : public query::TpchQ3InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ3CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ3Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ3OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ3Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ3LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ3Lineitems(consumer);
  }
};

Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q3_COST_MODEL",
              "config/q3_aux_cost_model.json",
              core::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q3Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ3Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q3_aux", "sd", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
