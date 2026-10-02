// MySQL plugin runtime for TPC-H Q15: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q15/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ15Suppliers(
    const std::function<void(const query::TpchQ15SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_phone FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  TypedText(cols, 3)});
      });
}

void ScanTpchQ15Lineitems(
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 3),
                  TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1), 2,
                  TypedDecimalRaw(cols, 2), 2});
      });
}

void ScanTpchQ15LineitemsForShipdateRange(
    int32_t start_days, int32_t end_days,
    const std::function<void(const query::TpchQ15LineitemInput &)> &consumer) {
  if (end_days <= start_days) {
    return;
  }
  const std::string sql =
      "SELECT l_suppkey, l_extendedprice, l_discount, l_shipdate FROM lineitem "
      "WHERE l_shipdate >= DATE_ADD('1970-01-01', INTERVAL " +
      std::to_string(start_days) +
      " DAY) AND l_shipdate < DATE_ADD('1970-01-01', INTERVAL " + std::to_string(end_days) +
      " DAY)";
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    consumer({TypedDate(cols, 3), TypedInt(cols, 0),
              TypedDecimalRaw(cols, 1), 2,
              TypedDecimalRaw(cols, 2), 2});
  });
}

} // namespace scans

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15Lineitems(consumer);
  }
  void ScanLineitemsForShipdateRange(
      int32_t start_days, int32_t end_days,
      const std::function<void(const query::TpchQ15LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ15LineitemsForShipdateRange(start_days, end_days, consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ15SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ15Suppliers(consumer);
  }
};

Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q15_COST_MODEL",
              "config/q15_aux_cost_model.json",
              query::TpchQ15Layout::kAccelerator,
              query::TpchQ15Config{mysql_tbl::kLineitem, mysql_tbl::kSupplier});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q15Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ15Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q15_aux", "d", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
