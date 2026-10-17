// MySQL plugin runtime for TPC-H Q2: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q2/mysql/aux/module.hpp"
#include "tpch/q2/cost_model.hpp"
#include "tpch/q2/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsTable() {
  return {"p_partkey, p_mfgr, p_type, p_size", "part", "", "p_partkey"};
}

query::TpchQ2PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3))};
}

SplitTable SuppliersTable() {
  return {"s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment", "supplier",
          "", "s_suppkey"};
}

query::TpchQ2SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2), TypedInt(cols, 3),
          TypedText(cols, 4), TypedDecimalRaw(cols, 5), TypedText(cols, 6)};
}

SplitTable PartsuppsTable() {
  return {"ps_partkey, ps_suppkey, ps_supplycost", "partsupp", "", "ps_partkey"};
}

query::TpchQ2PartsuppInput DecodePartsupp(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedDecimalRaw(cols, 2)};
}

void ScanTpchQ2Nations(
    const std::function<void(const query::TpchQ2NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ2NationInput{TypedInt(cols, 0),
                                        TypedText(cols, 1),
                                        TypedInt(cols, 2)};
      },
      [&](query::TpchQ2NationInput &&input) { consumer(input); });
}

void ScanTpchQ2Regions(
    const std::function<void(const query::TpchQ2RegionInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT r_regionkey, r_name FROM region",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ2RegionInput{TypedInt(cols, 0),
                                        TypedText(cols, 1)};
      },
      [&](query::TpchQ2RegionInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ2Scan final : public query::TpchQ2InitialScan {
public:
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2PartInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ2PartInput>(services, scans::PartsTable(), partitions,
                                                    scans::DecodePart, sink);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2SupplierInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ2SupplierInput>(
        services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
  void ScanPartsuppsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ2PartsuppInput &)> &sink) override {
    split_scan::Partitioned<query::TpchQ2PartsuppInput>(
        services, scans::PartsuppsTable(), partitions, scans::DecodePartsupp, sink);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ2NationInput &)> &consumer)
      override {
    scans::ScanTpchQ2Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ2RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ2Regions(consumer);
  }
};

Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan> &Q2Runtime() {
  static Runtime<query::TpchQ2Module, query::TpchQ2Config, TpchQ2Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q2_COST_MODEL",
              core::kTpchQ2CostModel, core::TpchQ2Layout::kAccelerator,
              query::TpchQ2Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kPartsupp, mysql_tbl::kNation, mysql_tbl::kRegion});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q2Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ2Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q2_aux", "iss", &ExecuteQuery,
    /*null_first_arg_yields_empty=*/true});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
