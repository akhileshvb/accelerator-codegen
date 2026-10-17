// MySQL plugin runtime for TPC-H Q11: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q11/mysql/aux/module.hpp"
#include "tpch/q11/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsuppTable() {
  return {"ps_partkey, ps_suppkey, ps_availqty, ps_supplycost", "partsupp", "", "ps_partkey"};
}

query::TpchQ11PartsuppInput DecodePartsupp(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2), TypedDecimalRaw(cols, 3), 2};
}

SplitTable SuppliersTable() {
  return {"s_suppkey, s_nationkey", "supplier", "", "s_suppkey"};
}

query::TpchQ11SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1)};
}

void ScanTpchQ11NationsSerial(
    const std::function<void(const query::TpchQ11NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name FROM nation",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ11NationInput{TypedInt(cols, 0),
                                         TypedText(cols, 1)};
      },
      [&](query::TpchQ11NationInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ11Scan final : public query::TpchQ11InitialScan {
public:
  void ScanPartsupp(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ11PartsuppInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ11PartsuppInput>(
        services, scans::PartsuppTable(), partitions, scans::DecodePartsupp, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ11SupplierInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ11SupplierInput>(
        services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ11NationInput &)> &consumer)
      override {
    scans::ScanTpchQ11NationsSerial(consumer);
  }
};

Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan> &
Q11Runtime() {
  static Runtime<query::TpchQ11Module, query::TpchQ11Config, TpchQ11Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q11_COST_MODEL",
              core::kTpchQ11CostModel,
              query::TpchQ11Layout::kAccelerator,
              query::TpchQ11Config{mysql_tbl::kPartsupp, mysql_tbl::kSupplier, mysql_tbl::kNation});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q11Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ11Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q11_aux", "sn", &ExecuteQuery,
    /*null_first_arg_yields_empty=*/true});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
