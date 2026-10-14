// MySQL plugin runtime for TPC-H Q7: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q7/mysql/aux/module.hpp"
#include "tpch/q7/cost_model.hpp"
#include "tpch/q7/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ7Nations(
    const std::function<void(const query::TpchQ7NationInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT n_nationkey, n_name FROM nation",
      [&](const std::vector<std::string> &cols) {
        consumer({static_cast<int32_t>(TypedInt(cols, 0)),
                  TypedText(cols, 1)});
      });
}

void ScanTpchQ7Suppliers(
    const std::function<void(const query::TpchQ7SupplierInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ7Customers(
    const std::function<void(const query::TpchQ7CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_nationkey FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  static_cast<int32_t>(TypedInt(cols, 1))});
      });
}

void ScanTpchQ7Orders(
    const std::function<void(const query::TpchQ7OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1)});
      });
}

void ScanTpchQ7Lineitems(
    const std::function<void(const query::TpchQ7LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate "
      "FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDecimalRaw(cols, 2), 2,
                  TypedDecimalRaw(cols, 3), 2,
                  TypedDate(cols, 4)});
      });
}

} // namespace scans

class TpchQ7Scan final : public query::TpchQ7InitialScan {
public:
  void ScanNations(
      const std::function<void(const query::TpchQ7NationInput &)> &consumer)
      override {
    scans::ScanTpchQ7Nations(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ7SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ7Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ7CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ7Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ7OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ7Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ7LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ7Lineitems(consumer);
  }
};

Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan> &Q7Runtime() {
  static Runtime<query::TpchQ7Module, query::TpchQ7Config, TpchQ7Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q7_COST_MODEL",
              core::kTpchQ7CostModel, core::TpchQ7Layout::kAccelerator,
              query::TpchQ7Config{mysql_tbl::kNation, mysql_tbl::kSupplier, mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q7Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ7Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q7_aux", "ss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
