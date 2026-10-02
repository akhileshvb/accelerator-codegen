// MySQL plugin runtime for TPC-H Q17: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q17/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ17Parts(
    const std::function<void(const query::TpchQ17PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2)});
      });
}

void ScanTpchQ17Lineitems(
    const std::function<void(const query::TpchQ17LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1),
                  TypedDecimalRaw(cols, 2)});
      });
}

void ScanTpchQ17PartQuantityAggregatesForPart(
    int64_t partkey,
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  const std::string sql =
      "SELECT CAST(ROUND(l_quantity * 100) AS SIGNED), "
      "SUM(CAST(ROUND(l_extendedprice * 100) AS SIGNED)), "
      "CAST(COUNT(*) AS SIGNED) "
      "FROM lineitem WHERE l_partkey = " +
      std::to_string(partkey) + " GROUP BY 1";
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    consumer({partkey, TypedInt(cols, 0),
              TypedInt(cols, 1), TypedInt(cols, 2)});
  });
}

} // namespace scans

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ17PartInput &)> &consumer)
      override {
    scans::ScanTpchQ17Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ17LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ17Lineitems(consumer);
  }
  bool ScanPartQuantityAggregatesForPart(
      int64_t partkey,
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregatesForPart(partkey, consumer);
    return true;
  }
};

Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q17_COST_MODEL",
              "config/q17_aux_cost_model.json",
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{mysql_tbl::kPart, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q17Runtime().Execute(arguments, out);
  if (out.find_first_not_of(" \t\r\n") == std::string::npos) {
    out = "NULL\n";
  }
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ17Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q17_aux", "ss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
