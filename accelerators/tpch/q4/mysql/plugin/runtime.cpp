// MySQL plugin runtime for TPC-H Q4: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q4/mysql/aux/module.hpp"
#include "tpch/q4/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable OrdersTable() {
  return {"o_orderkey, o_orderdate, o_orderpriority, "
          "CASE WHEN o_orderpriority IS NULL THEN 1 ELSE 0 END",
          "orders", "o_orderkey IS NOT NULL AND o_orderdate IS NOT NULL", "o_orderkey"};
}

query::TpchQ4OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDate(cols, 1), TypedText(cols, 2), TypedInt(cols, 3) != 0};
}

SplitTable LineitemsTable() {
  return {"l_orderkey, l_commitdate, l_receiptdate", "lineitem",
          "l_orderkey IS NOT NULL AND l_commitdate < l_receiptdate", "l_orderkey"};
}

query::TpchQ4LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDate(cols, 1), TypedDate(cols, 2)};
}

} // namespace scans

class TpchQ4Scan final : public query::TpchQ4InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ4OrderInput &)> &consumer)
      override {
    split_scan::Serial<query::TpchQ4OrderInput>(scans::OrdersTable(), scans::DecodeOrder, consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ4LineitemInput &)> &consumer)
      override {
    split_scan::Serial<query::TpchQ4LineitemInput>(scans::LineitemsTable(), scans::DecodeLineitem,
                                                   consumer);
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ4OrderInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ4OrderInput>(services, scans::OrdersTable(), partitions,
                                                     scans::DecodeOrder, sink);
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ4LineitemInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ4LineitemInput>(services, scans::LineitemsTable(),
                                                        partitions, scans::DecodeLineitem, sink);
  }
};

Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan> &Q4Runtime() {
  static Runtime<query::TpchQ4Module, query::TpchQ4Config, TpchQ4Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q4_COST_MODEL",
              core::kTpchQ4CostModel, query::TpchQ4Layout::kAccelerator,
              query::TpchQ4Config{mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q4Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ4Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q4_aux", "d", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
