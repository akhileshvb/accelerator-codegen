// MySQL plugin runtime for TPC-H Q13: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
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

SplitTable CustomersTable() {
  return {"c_custkey", "customer", "c_custkey IS NOT NULL", "c_custkey"};
}

query::TpchQ13CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0)};
}

SplitTable OrdersTable() {
  return {"o_orderkey, o_custkey, o_comment", "orders",
          "o_orderkey IS NOT NULL AND o_custkey IS NOT NULL AND o_comment IS NOT NULL",
          "o_orderkey"};
}

query::TpchQ13OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedText(cols, 2)};
}

} // namespace scans

class TpchQ13Scan final : public query::TpchQ13InitialScan {
public:
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ13CustomerInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ13CustomerInput>(
        services, scans::CustomersTable(), partitions, scans::DecodeCustomer, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ13OrderInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ13OrderInput>(
        services, scans::OrdersTable(), partitions, scans::DecodeOrder, sink);
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
