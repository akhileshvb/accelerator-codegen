// MySQL plugin runtime for TPC-H Q6: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include <stdexcept>

#include "prims/decimal_text.hpp"
#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q6/mysql/aux/module.hpp"
#include "tpch/q6/cost_model.hpp"
#include "tpch/q6/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

// One SQL GROUP BY: the engine aggregates, the process receives one row per
// (shipdate, discount, quantity) cell with the exact revenue sum (text).
SplitTable CellAggregatesTable() {
  return {"l_shipdate, l_discount, l_quantity, count(*), sum(l_extendedprice * l_discount)",
          "lineitem", "", "l_orderkey", "l_shipdate, l_discount, l_quantity"};
}

query::TpchQ6CellAggregate DecodeCellAggregate(const std::vector<std::string> &cols) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  query::TpchQ6CellAggregate cell;
  cell.shipdate_days = TypedDate(cols, 0);
  __int128 discount = 0, quantity = 0;
  if (!dec::Parse(TypedText(cols, 1), 2, discount) ||
      !dec::Parse(TypedText(cols, 2), 2, quantity) ||
      !dec::Parse(TypedText(cols, 4), 4, cell.revenue_raw))
    throw std::runtime_error("TPC-H Q6: unparsable decimal");
  cell.discount_raw = static_cast<int64_t>(discount);
  cell.quantity_raw = static_cast<int64_t>(quantity);
  cell.count = TypedInt(cols, 3);
  return cell;
}

} // namespace scans

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanCellAggregates(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ6CellAggregate &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ6CellAggregate>(
        services, scans::CellAggregatesTable(), partitions, scans::DecodeCellAggregate, sink);
  }
};

Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q6_COST_MODEL",
              core::kTpchQ6CostModel,
              core::TpchQ6Layout::kAccelerator, query::TpchQ6Config{mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q6Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ6Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q6_aux", "dnn", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
