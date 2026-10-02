// MySQL plugin runtime for TPC-H Q1: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q1/mysql/aux/module.hpp"
#include "tpch/q1/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, l_tax, "
      "l_returnflag, l_linestatus FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2,
                  TypedDecimalRaw(cols, 4), 2,
                  TypedChar(cols, 5),
                  TypedChar(cols, 6)});
      });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
};

Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q1_COST_MODEL",
              "config/q1_aux_cost_model.json",
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q1Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ1Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q1_aux", "i", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
