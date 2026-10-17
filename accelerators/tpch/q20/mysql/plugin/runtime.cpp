// MySQL plugin runtime for TPC-H Q20: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
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

SplitTable LineitemsTable() {
  return {"l_shipdate, l_partkey, l_suppkey, l_quantity", "lineitem",
          "l_shipdate IS NOT NULL AND l_partkey IS NOT NULL AND "
          "l_suppkey IS NOT NULL AND l_quantity IS NOT NULL",
          "l_orderkey"};
}

query::TpchQ20LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedDate(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2), TypedDecimalRaw(cols, 3)};
}

SplitTable PartsTable() {
  return {"p_partkey, p_name", "part", "p_partkey IS NOT NULL AND p_name IS NOT NULL",
          "p_partkey"};
}

query::TpchQ20PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1)};
}

SplitTable PartsuppsTable() {
  return {"ps_partkey, ps_suppkey, ps_availqty", "partsupp",
          "ps_partkey IS NOT NULL AND ps_suppkey IS NOT NULL AND "
          "ps_availqty IS NOT NULL",
          "ps_partkey"};
}

query::TpchQ20PartsuppInput DecodePartsupp(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2)};
}

SplitTable SuppliersTable() {
  return {"s_suppkey, s_name, s_address, s_nationkey", "supplier",
          "s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL", "s_suppkey"};
}

query::TpchQ20SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3))};
}

void ScanTpchQ20Nations(
    const std::function<void(const query::TpchQ20NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ20NationInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                         TypedText(cols, 1)};
      },
      [&](query::TpchQ20NationInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ20Scan final : public query::TpchQ20InitialScan {
public:
  void ScanLineitems(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20LineitemInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ20LineitemInput>(
        services, scans::LineitemsTable(), partitions, scans::DecodeLineitem, sink);
  }
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20PartInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ20PartInput>(
        services, scans::PartsTable(), partitions, scans::DecodePart, sink);
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20PartsuppInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ20PartsuppInput>(
        services, scans::PartsuppsTable(), partitions, scans::DecodePartsupp, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ20SupplierInput &)> &sink)
      override {
    split_scan::Partitioned<query::TpchQ20SupplierInput>(
        services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
  void ScanNationsSerial(
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
