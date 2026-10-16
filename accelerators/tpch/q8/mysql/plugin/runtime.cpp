// MySQL plugin runtime for TPC-H Q8: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

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

void ScanTpchQ8Parts(
    const std::function<void(const query::TpchQ8PartInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT p_partkey, p_type FROM part",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8PartInput{TypedInt(cols, 0),
                                      TypedText(cols, 1)};
      },
      [&](query::TpchQ8PartInput &&input) { consumer(input); });
}

void ScanTpchQ8Suppliers(
    const std::function<void(const query::TpchQ8SupplierInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT s_suppkey, s_nationkey FROM supplier",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8SupplierInput{TypedInt(cols, 0),
                                          static_cast<int32_t>(TypedInt(cols, 1))};
      },
      [&](query::TpchQ8SupplierInput &&input) { consumer(input); });
}

void ScanTpchQ8Customers(
    const std::function<void(const query::TpchQ8CustomerInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT c_custkey, c_nationkey FROM customer",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8CustomerInput{TypedInt(cols, 0),
                                          static_cast<int32_t>(TypedInt(cols, 1))};
      },
      [&](query::TpchQ8CustomerInput &&input) { consumer(input); });
}

void ScanTpchQ8Orders(
    const std::function<void(const query::TpchQ8OrdersInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT o_orderkey, o_custkey, o_orderdate FROM orders",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8OrdersInput{TypedInt(cols, 0),
                                        TypedInt(cols, 1),
                                        TypedDate(cols, 2)};
      },
      [&](query::TpchQ8OrdersInput &&input) { consumer(input); });
}

void ScanTpchQ8Lineitems(
    const std::function<void(const query::TpchQ8LineitemInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT l_partkey, l_orderkey, l_suppkey, l_extendedprice, l_discount "
      "FROM lineitem",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ8LineitemInput{TypedInt(cols, 0),
                                          TypedInt(cols, 1),
                                          TypedInt(cols, 2),
                                          TypedDecimalRaw(cols, 3),
                                          TypedDecimalRaw(cols, 4)};
      },
      [&](query::TpchQ8LineitemInput &&input) { consumer(input); });
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
  void ScanParts(
      const std::function<void(const query::TpchQ8PartInput &)> &consumer)
      override {
    scans::ScanTpchQ8Parts(consumer);
  }
  void ScanSuppliers(
      const std::function<void(const query::TpchQ8SupplierInput &)> &consumer)
      override {
    scans::ScanTpchQ8Suppliers(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ8CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ8Customers(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ8OrdersInput &)> &consumer)
      override {
    scans::ScanTpchQ8Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ8LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ8Lineitems(consumer);
  }
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
