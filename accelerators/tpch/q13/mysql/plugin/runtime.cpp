// MySQL plugin runtime for TPC-H Q13: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q13/mysql/aux/module.hpp"
#include "tpch/q13/cost_model.hpp"
#include "tpch/q13/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ13Customers(
    const std::function<void(const query::TpchQ13CustomerInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT c_custkey FROM customer WHERE c_custkey IS NOT NULL",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ13CustomerInput{TypedInt(cols, 0)};
      },
      [&](query::TpchQ13CustomerInput &&input) { consumer(input); });
}

void ScanTpchQ13Orders(
    const std::function<void(const query::TpchQ13OrderInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT o_orderkey, o_custkey, o_comment FROM orders "
      "WHERE o_orderkey IS NOT NULL AND o_custkey IS NOT NULL AND o_comment IS NOT NULL",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ13OrderInput{TypedInt(cols, 0), TypedInt(cols, 1), TypedText(cols, 2)};
      },
      [&](query::TpchQ13OrderInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ13Scan final : public query::TpchQ13InitialScan {
public:
  void ScanCustomers(
      const std::function<void(const query::TpchQ13CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ13Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ13OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ13Orders(consumer);
  }
};

Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan> &
Q13Runtime() {
  static Runtime<query::TpchQ13Module, query::TpchQ13Config, TpchQ13Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q13_COST_MODEL",
              core::kTpchQ13CostModel,
              core::TpchQ13Layout::kAccelerator, query::TpchQ13Config{mysql_tbl::kCustomer, mysql_tbl::kOrders});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q13Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ13Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q13_aux", "s", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
