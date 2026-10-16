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
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name FROM nation",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ7NationInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                        TypedText(cols, 1)};
      },
      [&](query::TpchQ7NationInput &&input) { consumer(input); });
}

SplitTable SuppliersTable() { return {"s_suppkey, s_nationkey", "supplier", "", "s_suppkey"}; }

query::TpchQ7SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1))};
}

SplitTable CustomersTable() { return {"c_custkey, c_nationkey", "customer", "", "c_custkey"}; }

query::TpchQ7CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1))};
}

SplitTable OrdersTable() { return {"o_orderkey, o_custkey", "orders", "", "o_orderkey"}; }

query::TpchQ7OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1)};
}

SplitTable LineitemsTable() {
  return {"l_orderkey, l_suppkey, l_extendedprice, l_discount, l_shipdate", "lineitem", "",
          "l_orderkey"};
}

query::TpchQ7LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0),        TypedInt(cols, 1), TypedDecimalRaw(cols, 2), 2,
          TypedDecimalRaw(cols, 3), 2,                 TypedDate(cols, 4)};
}

template <class Input, class Decode>
void ScanSerial(const SplitTable &table, Decode decode,
                const std::function<void(const Input &)> &consumer) {
  ScanTypedTuplesDecoded(SelectSql(table), decode, [&](Input &&input) { consumer(input); });
}

template <class Input, class Decode>
void ScanPartitioned(const core::AuxEngineServices &services, const SplitTable &table,
                     std::size_t partitions, Decode decode,
                     const std::function<void(std::size_t, const Input &)> &sink) {
  ScanTypedTuplesPartitioned(services, table, partitions,
                             [&](std::size_t partition, const std::vector<std::string> &cols) {
                               sink(partition, decode(cols));
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
    scans::ScanSerial(scans::SuppliersTable(), scans::DecodeSupplier, consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ7CustomerInput &)> &consumer)
      override {
    scans::ScanSerial(scans::CustomersTable(), scans::DecodeCustomer, consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ7OrderInput &)> &consumer)
      override {
    scans::ScanSerial(scans::OrdersTable(), scans::DecodeOrder, consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ7LineitemInput &)> &consumer)
      override {
    scans::ScanSerial(scans::LineitemsTable(), scans::DecodeLineitem, consumer);
  }
  void ScanSuppliersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7SupplierInput &)> &sink)
      override {
    scans::ScanPartitioned(services, scans::SuppliersTable(), partitions,
                           scans::DecodeSupplier, sink);
  }
  void ScanCustomersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7CustomerInput &)> &sink)
      override {
    scans::ScanPartitioned(services, scans::CustomersTable(), partitions,
                           scans::DecodeCustomer, sink);
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7OrderInput &)> &sink)
      override {
    scans::ScanPartitioned(services, scans::OrdersTable(), partitions, scans::DecodeOrder,
                           sink);
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ7LineitemInput &)> &sink)
      override {
    scans::ScanPartitioned(services, scans::LineitemsTable(), partitions,
                           scans::DecodeLineitem, sink);
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
