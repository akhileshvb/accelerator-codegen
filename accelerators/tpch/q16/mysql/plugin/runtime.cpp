// MySQL plugin runtime for TPC-H Q16: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q16/mysql/aux/module.hpp"
#include "tpch/q16/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable SuppliersTable() {
  return {"s_suppkey, s_comment", "supplier", "", "s_suppkey"};
}

query::TpchQ16SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1)};
}

SplitTable PartsTable() {
  return {"p_partkey, p_brand, p_type, p_size", "part", "", "p_partkey"};
}

query::TpchQ16PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3))};
}

SplitTable PartsuppTable() {
  return {"ps_partkey, ps_suppkey", "partsupp", "", "ps_partkey"};
}

query::TpchQ16PartsuppInput DecodePartsupp(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1)};
}

} // namespace scans

class TpchQ16Scan final : public query::TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ16SupplierInput &)> &consumer)
      override {
    split_scan::Serial<query::TpchQ16SupplierInput>(scans::SuppliersTable(),
                                                    scans::DecodeSupplier, consumer);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ16SupplierInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ16SupplierInput>(
        services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ16PartInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ16PartInput>(services, scans::PartsTable(), partitions,
                                                     scans::DecodePart, sink);
  }
  void ScanPartsuppPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ16PartsuppInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ16PartsuppInput>(
        services, scans::PartsuppTable(), partitions, scans::DecodePartsupp, sink);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ16PartInput &)> &consumer)
      override {
    split_scan::Serial<query::TpchQ16PartInput>(scans::PartsTable(), scans::DecodePart, consumer);
  }
  void ScanPartsupp(
      const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer)
      override {
    split_scan::Serial<query::TpchQ16PartsuppInput>(scans::PartsuppTable(),
                                                    scans::DecodePartsupp, consumer);
  }
};

Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan> &
Q16Runtime() {
  static Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q16_COST_MODEL",
              core::kTpchQ16CostModel,
              query::TpchQ16Layout::kAccelerator,
              query::TpchQ16Config{mysql_tbl::kPart, mysql_tbl::kPartsupp, mysql_tbl::kSupplier});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q16Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ16Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q16_aux", "ssssssssss", &ExecuteQuery,
    /*null_first_arg_yields_empty=*/true});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
