// MySQL plugin runtime for TPC-H Q10: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q10/mysql/aux/module.hpp"
#include "tpch/q10/cost_model.hpp"
#include "tpch/q10/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable CustomersTable() {
  return {"c_custkey, c_name, c_acctbal, c_nationkey, c_address, c_phone, c_comment", "customer",
          "", "c_custkey"};
}

query::TpchQ10CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedDecimalRaw(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3)), TypedText(cols, 4), TypedText(cols, 5),
          TypedText(cols, 6)};
}

SplitTable OrdersTable() {
  return {"o_orderkey, o_custkey, o_orderdate", "orders", "", "o_orderkey"};
}

query::TpchQ10OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2)};
}

SplitTable LineitemsTable() {
  return {"l_orderkey, l_extendedprice, l_discount, l_returnflag", "lineitem", "", "l_orderkey"};
}

query::TpchQ10LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDecimalRaw(cols, 1), 2, TypedDecimalRaw(cols, 2), 2,
          TypedText(cols, 3) == "R"};
}

void ScanTpchQ10NationsSerial(
    const std::function<void(const query::TpchQ10NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name FROM nation",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ10NationInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                         TypedText(cols, 1)};
      },
      [&](query::TpchQ10NationInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ10Scan final : public query::TpchQ10InitialScan {
public:
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ10NationInput &)> &consumer)
      override {
    scans::ScanTpchQ10NationsSerial(consumer);
  }
  void ScanCustomers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10CustomerInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ10CustomerInput>(
        services, scans::CustomersTable(), partitions, scans::DecodeCustomer, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10OrderInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ10OrderInput>(services, scans::OrdersTable(), partitions,
                                                      scans::DecodeOrder, sink);
  }
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ10LineitemInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ10LineitemInput>(
        services, scans::LineitemsTable(), partitions, scans::DecodeLineitem, sink);
  }
};

Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan> &
Q10Runtime() {
  static Runtime<query::TpchQ10Module, query::TpchQ10Config, TpchQ10Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q10_COST_MODEL",
              core::kTpchQ10CostModel,
              core::TpchQ10Layout::kAccelerator,
              query::TpchQ10Config{mysql_tbl::kCustomer, mysql_tbl::kNation, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q10Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ10Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q10_aux", "d", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
