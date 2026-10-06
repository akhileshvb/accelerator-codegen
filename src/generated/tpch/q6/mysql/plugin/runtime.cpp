// MySQL plugin runtime for TPC-H Q6: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include <stdexcept>

#include "accelerator_codegen/stateful/core/decimal_text_g1.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q6/mysql/aux/module.hpp"
#include "tpch/q6/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ6Lineitems(
    const std::function<void(const query::TpchQ6LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, l_extendedprice FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2});
      });
}

// One SQL GROUP BY: the engine aggregates, the process receives one row per
// (shipdate, discount, quantity) cell with the exact revenue sum (text).
void ScanTpchQ6CellAggregates(
    const std::function<void(const query::TpchQ6CellAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  ScanTypedTuples(
      "SELECT l_shipdate, l_discount, l_quantity, count(*), "
      "sum(l_extendedprice * l_discount) FROM lineitem "
      "GROUP BY l_shipdate, l_discount, l_quantity",
      [&](const std::vector<std::string> &cols) {
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
        consumer(cell);
      });
}

} // namespace scans

class TpchQ6Scan final : public query::TpchQ6InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ6LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ6Lineitems(consumer);
  }
  void ScanCellAggregates(
      const std::function<void(const query::TpchQ6CellAggregate &)> &consumer)
      override {
    scans::ScanTpchQ6CellAggregates(consumer);
  }
};

Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan> &Q6Runtime() {
  static Runtime<query::TpchQ6Module, query::TpchQ6Config, TpchQ6Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q6_COST_MODEL",
              "config/q6_aux_cost_model.json",
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
    "mysql_accel_tpch_q6_aux", "dff", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
