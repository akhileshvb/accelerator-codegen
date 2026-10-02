// MySQL plugin runtime for TPC-H Q22: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q22/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ22Customers(
    const std::function<void(const query::TpchQ22CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_phone, c_acctbal FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedDecimalRaw(cols, 2), 2});
      });
}

void ScanTpchQ22Orders(
    const std::function<void(const query::TpchQ22OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_custkey FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0)});
      });
}

} // namespace scans

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ22CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ22Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ22OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ22Orders(consumer);
  }
};

Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q22_COST_MODEL",
              "config/q22_aux_cost_model.json",
              query::TpchQ22Layout::kAccelerator,
              query::TpchQ22Config{mysql_tbl::kOrders, mysql_tbl::kCustomer});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q22Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ22Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q22_aux", "sssssss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
