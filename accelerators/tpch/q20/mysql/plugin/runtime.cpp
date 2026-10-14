// MySQL plugin runtime for TPC-H Q20: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q20/mysql/aux/module.hpp"
#include "tpch/q20/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ20Lineitems(
    const std::function<void(const query::TpchQ20LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_shipdate, l_partkey, l_suppkey, l_quantity FROM lineitem "
      "WHERE l_shipdate IS NOT NULL AND l_partkey IS NOT NULL AND "
      "l_suppkey IS NOT NULL AND l_quantity IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedDate(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2),
                  TypedDecimalRaw(cols, 3)});
      });
}

void ScanTpchQ20Parts(
    const std::function<void(const query::TpchQ20PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_name FROM part "
      "WHERE p_partkey IS NOT NULL AND p_name IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ20Partsupps(
    const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT ps_partkey, ps_suppkey, ps_availqty FROM partsupp "
      "WHERE ps_partkey IS NOT NULL AND ps_suppkey IS NOT NULL AND "
      "ps_availqty IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedInt(cols, 2)});
      });
}

void ScanTpchQ20Suppliers(
    const std::function<void(const query::TpchQ20SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_name, s_address, s_nationkey FROM supplier "
      "WHERE s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

void ScanTpchQ20Nations(
    const std::function<void(const query::TpchQ20NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

} // namespace scans

class TpchQ20Scan final : public query::TpchQ20InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ20LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ20Lineitems(consumer);
  }
  void ScanParts(
      const std::function<void(const query::TpchQ20PartInput &)> &consumer)
      override {
    scans::ScanTpchQ20Parts(consumer);
  }
  void ScanPartsupps(
      const std::function<void(const query::TpchQ20PartsuppInput &)> &consumer)
      override {
    scans::ScanTpchQ20Partsupps(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ20SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ20Suppliers(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ20NationInput &)> &consumer)
      override {
    scans::ScanTpchQ20Nations(consumer);
  }
};

Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan> &
Q20Runtime() {
  static Runtime<query::TpchQ20Module, query::TpchQ20Config, TpchQ20Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q20_COST_MODEL",
              core::kTpchQ20CostModel,
              query::TpchQ20Layout::kAccelerator,
              query::TpchQ20Config{mysql_tbl::kLineitem, mysql_tbl::kPart, mysql_tbl::kPartsupp, mysql_tbl::kSupplier, mysql_tbl::kNation});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q20Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ20Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q20_aux", "dss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
