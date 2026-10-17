// MySQL plugin runtime for TPC-H Q15: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q15/mysql/aux/module.hpp"
#include "tpch/q15/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable SuppliersTable() {
  return {"s_suppkey, s_name, s_address, s_phone", "supplier", "", "s_suppkey"};
}

query::TpchQ15SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2), TypedText(cols, 3)};
}

SplitTable LineitemsTable() {
  return {"l_suppkey, l_extendedprice, l_discount, l_shipdate", "lineitem", "", "l_orderkey"};
}

query::TpchQ15LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedDate(cols, 3), TypedInt(cols, 0), TypedDecimalRaw(cols, 1), 2,
          TypedDecimalRaw(cols, 2), 2};
}

} // namespace scans

class TpchQ15Scan final : public query::TpchQ15InitialScan {
public:
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ15LineitemInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ15LineitemInput>(
        services, scans::LineitemsTable(), partitions, scans::DecodeLineitem, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ15SupplierInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ15SupplierInput>(
        services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
};

Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan> &
Q15Runtime() {
  static Runtime<query::TpchQ15Module, query::TpchQ15Config, TpchQ15Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q15_COST_MODEL",
              core::kTpchQ15CostModel,
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
