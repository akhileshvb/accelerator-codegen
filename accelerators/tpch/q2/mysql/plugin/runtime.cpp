// MySQL plugin runtime for TPC-H Q2: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

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

void ScanTpchQ2Parts(
    const std::function<void(const query::TpchQ2PartInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT p_partkey, p_mfgr, p_type, p_size FROM part",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ2PartInput{TypedInt(cols, 0),
                                      TypedText(cols, 1),
                                      TypedText(cols, 2),
                                      static_cast<int32_t>(TypedInt(cols, 3))};
      },
      [&](query::TpchQ2PartInput &&input) { consumer(input); });
}

void ScanTpchQ2Suppliers(
    const std::function<void(const query::TpchQ2SupplierInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, "
      "s_comment FROM supplier",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ2SupplierInput{TypedInt(cols, 0),
                                          TypedText(cols, 1),
                                          TypedText(cols, 2),
                                          TypedInt(cols, 3),
                                          TypedText(cols, 4),
                                          TypedDecimalRaw(cols, 5),
                                          TypedText(cols, 6)};
      },
      [&](query::TpchQ2SupplierInput &&input) { consumer(input); });
}

void ScanTpchQ2Partsupps(
    const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT ps_partkey, ps_suppkey, ps_supplycost FROM partsupp",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ2PartsuppInput{TypedInt(cols, 0),
                                          TypedInt(cols, 1),
                                          TypedDecimalRaw(cols, 2)};
      },
      [&](query::TpchQ2PartsuppInput &&input) { consumer(input); });
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
  void ScanParts(
      const std::function<void(const query::TpchQ2PartInput &)> &consumer)
      override {
    scans::ScanTpchQ2Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ2SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ2Suppliers(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ2PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ2Partsupps(consumer);
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
