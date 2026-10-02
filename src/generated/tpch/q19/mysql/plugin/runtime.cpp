// MySQL plugin runtime for TPC-H Q19: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q19/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ19Lineitems(
    const std::function<void(const query::TpchQ19LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, l_extendedprice, l_discount, l_shipmode, "
      "l_shipinstruct FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1),
                  TypedDecimalRaw(cols, 2),
                  TypedDecimalRaw(cols, 3),
                  TypedText(cols, 4),
                  TypedText(cols, 5)});
      });
}

bool LookupTpchQ19Part(int64_t partkey, query::TpchQ19PartInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT p_brand, p_container, p_size FROM part WHERE p_partkey = " +
      std::to_string(partkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {partkey, TypedText(cols, 0), TypedText(cols, 1),
           static_cast<int32_t>(TypedInt(cols, 2))};
    found = true;
  });
  return found;
}

} // namespace scans

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ19LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ19Lineitems(consumer);
  }
  bool LookupPart(int64_t partkey, query::TpchQ19PartInput &out) override {
    return scans::LookupTpchQ19Part(partkey, out);
  }
};

Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q19_COST_MODEL",
              "config/q19_aux_cost_model.json",
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{mysql_tbl::kLineitem, mysql_tbl::kPart});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q19Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ19Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q19_aux", "sisisi", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
