// MySQL plugin runtime for TPC-H Q22: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q22/mysql/aux/module.hpp"
#include "tpch/q22/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable CustomersTable() {
  return {"c_custkey, c_phone, c_acctbal", "customer", "", "c_custkey"};
}

query::TpchQ22CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedDecimalRaw(cols, 2), 2};
}

// Orders per customer, aggregated in SQL. Partitions are ranges of o_custkey,
// so each custkey comes back once.
SplitTable OrderCountsTable() {
  return {"o_custkey, CAST(COUNT(*) AS SIGNED)", "orders", "o_custkey IS NOT NULL", "o_custkey",
          "o_custkey"};
}

query::TpchQ22OrderCount DecodeOrderCount(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1)};
}

} // namespace scans

class TpchQ22Scan final : public query::TpchQ22InitialScan {
public:
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ22CustomerInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ22CustomerInput>(
        services, scans::CustomersTable(), partitions, scans::DecodeCustomer, sink);
  }
  void ScanOrderCounts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ22OrderCount &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ22OrderCount>(
        services, scans::OrderCountsTable(), partitions, scans::DecodeOrderCount, sink);
  }
};

Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan> &
Q22Runtime() {
  static Runtime<query::TpchQ22Module, query::TpchQ22Config, TpchQ22Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q22_COST_MODEL",
              core::kTpchQ22CostModel,
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
