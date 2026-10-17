// MySQL plugin runtime for TPC-H Q8: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q8/mysql/aux/module.hpp"
#include "tpch/q8/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsTable() { return {"p_partkey, p_type", "part", "", "p_partkey"}; }

query::TpchQ8PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1)};
}

SplitTable SuppliersTable() { return {"s_suppkey, s_nationkey", "supplier", "", "s_suppkey"}; }

query::TpchQ8SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1))};
}

SplitTable CustomersTable() { return {"c_custkey, c_nationkey", "customer", "", "c_custkey"}; }

query::TpchQ8CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1))};
}

SplitTable OrdersTable() {
  return {"o_orderkey, o_custkey, o_orderdate", "orders", "", "o_orderkey"};
}

query::TpchQ8OrdersInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2)};
}

SplitTable LineitemsTable() {
  return {"l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount", "lineitem", "",
          "l_orderkey"};
}

query::TpchQ8LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2), TypedDecimalRaw(cols, 3),
          TypedDecimalRaw(cols, 4)};
}

void ScanTpchQ8Nations(
    const std::function<void(const query::TpchQ8NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8NationInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                        TypedText(cols, 1),
                                        static_cast<int32_t>(TypedInt(cols, 2))};
      },
      [&](query::TpchQ8NationInput &&input) { consumer(input); });
}

void ScanTpchQ8Regions(
    const std::function<void(const query::TpchQ8RegionInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT r_regionkey, r_name FROM region",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8RegionInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                        TypedText(cols, 1)};
      },
      [&](query::TpchQ8RegionInput &&input) { consumer(input); });
}

} // namespace scans

class TpchQ8Scan final : public query::TpchQ8InitialScan {
public:
  void ScanNations(
      const std::function<void(const query::TpchQ8NationInput &)> &consumer)
      override {
    scans::ScanTpchQ8Nations(consumer);
  }
  void ScanRegions(
      const std::function<void(const query::TpchQ8RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ8Regions(consumer);
  }
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ8PartInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::PartsTable(), partitions, scans::DecodePart, sink);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ8SupplierInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::SuppliersTable(), partitions, scans::DecodeSupplier, sink);
  }
  void ScanCustomersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ8CustomerInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::CustomersTable(), partitions, scans::DecodeCustomer, sink);
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ8OrdersInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::OrdersTable(), partitions, scans::DecodeOrder, sink);
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ8LineitemInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::LineitemsTable(), partitions, scans::DecodeLineitem, sink);
  }
};

Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan> &Q8Runtime() {
  static Runtime<query::TpchQ8Module, query::TpchQ8Config, TpchQ8Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q8_COST_MODEL",
              core::kTpchQ8CostModel, query::TpchQ8Layout::kAccelerator,
              query::TpchQ8Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem, mysql_tbl::kNation, mysql_tbl::kRegion});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q8Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ8Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q8_aux", "sss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
