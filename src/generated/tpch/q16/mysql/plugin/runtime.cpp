// MySQL plugin runtime for TPC-H Q16: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q16/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ16Suppliers(
    const std::function<void(const query::TpchQ16SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_comment FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ16Parts(
    const std::function<void(const query::TpchQ16PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_type, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ16Partsupp(
    const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey FROM partsupp",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1)});
      });
}

} // namespace scans

class TpchQ16Scan final : public query::TpchQ16InitialScan {
public:
  void ScanSuppliers(
      const std::function<void(const query::TpchQ16SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ16Suppliers(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ16PartInput &)> &consumer)
      override {
    scans::ScanTpchQ16Parts(consumer);
  }
  void ScanPartsupp(
      const std::function<void(const query::TpchQ16PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ16Partsupp(consumer);
  }
};

Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan> &
Q16Runtime() {
  static Runtime<query::TpchQ16Module, query::TpchQ16Config, TpchQ16Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q16_COST_MODEL",
              "config/q16_aux_cost_model.json",
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
