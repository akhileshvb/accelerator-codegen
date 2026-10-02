// MySQL plugin runtime for TPC-H Q5: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q5/mysql/aux/module.hpp"
#include "tpch/q5/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ5Regions(
    const std::function<void(const query::TpchQ5RegionInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT r_regionkey, r_name FROM region",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ5Nations(
    const std::function<void(const query::TpchQ5NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name, n_regionkey FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1),
                  static_cast<int32_t>(TypedInt(cols, 2))});
      });
}

void ScanTpchQ5Customers(
    const std::function<void(const query::TpchQ5CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ5Suppliers(
    const std::function<void(const query::TpchQ5SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ5Orders(
    const std::function<void(const query::TpchQ5OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2)});
      });
}

void ScanTpchQ5Lineitems(
    const std::function<void(const query::TpchQ5LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2});
      });
}

} // namespace scans

class TpchQ5Scan final : public query::TpchQ5InitialScan {
public:
  void ScanRegions(
      const std::function<void(const query::TpchQ5RegionInput &)> &consumer)
      override {
    scans::ScanTpchQ5Regions(consumer);
  }
  void ScanNations(
      const std::function<void(const query::TpchQ5NationInput &)> &consumer)
      override {
    scans::ScanTpchQ5Nations(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ5CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ5Customers(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ5SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ5Suppliers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ5OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ5Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ5LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ5Lineitems(consumer);
  }
};

Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan> &Q5Runtime() {
  static Runtime<query::TpchQ5Module, query::TpchQ5Config, TpchQ5Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q5_COST_MODEL",
              "config/q5_aux_cost_model.json", core::TpchQ5Layout::kAccelerator,
              query::TpchQ5Config{mysql_tbl::kRegion, mysql_tbl::kNation, mysql_tbl::kCustomer, mysql_tbl::kSupplier, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q5Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ5Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q5_aux", "sd", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
