// MySQL plugin runtime for TPC-H Q3: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q3/mysql/aux/module.hpp"
#include "tpch/q3/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

// customer ⨝ orders ⨝ lineitem, one row per lineitem, one order's rows
// consecutive. `restrict_sql` is an extra predicate (empty: every order).
void ScanTpchQ3OrderLines(
    const std::string &restrict_sql,
    const std::function<void(const query::TpchQ3OrderLineInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT o.o_orderkey, o.o_orderdate, o.o_shippriority, c.c_mktsegment, "
      "l.l_shipdate, l.l_extendedprice, l.l_discount "
      "FROM orders o JOIN customer c ON c.c_custkey = o.o_custkey "
      "JOIN lineitem l ON l.l_orderkey = o.o_orderkey" +
          restrict_sql + " ORDER BY o.o_orderkey",
      [](const std::vector<std::string> &cols) {
        query::TpchQ3OrderLineInput line;
        line.orderkey = TypedInt(cols, 0);
        line.orderdate_days = TypedDate(cols, 1);
        line.shippriority = static_cast<int32_t>(TypedInt(cols, 2));
        line.mktsegment = TypedText(cols, 3);
        line.shipdate_days = TypedDate(cols, 4);
        line.extended_price_raw = TypedDecimalRaw(cols, 5);
        line.discount_raw = TypedDecimalRaw(cols, 6);
        return line;
      },
      [&](query::TpchQ3OrderLineInput &&input) { consumer(input); });
}

// The three tables of the join, scanned separately by the partitioned build
// (inner joins: a NULL key joins nothing).
SplitTable CustomersTable() {
  return {"c_custkey, c_mktsegment", "customer",
          "c_custkey IS NOT NULL AND c_mktsegment IS NOT NULL", "c_custkey"};
}

query::TpchQ3CustomerInput DecodeCustomer(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1)};
}

SplitTable OrdersTable() {
  return {"o_orderkey, o_custkey, o_orderdate, o_shippriority", "orders",
          "o_orderkey IS NOT NULL AND o_custkey IS NOT NULL", "o_orderkey"};
}

query::TpchQ3OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3))};
}

SplitTable LineitemsTable() {
  return {"l_orderkey, l_shipdate, l_extendedprice, l_discount", "lineitem",
          "l_orderkey IS NOT NULL", "l_orderkey"};
}

query::TpchQ3LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDate(cols, 1), TypedDecimalRaw(cols, 2),
          TypedDecimalRaw(cols, 3)};
}

std::string KeyList(const std::vector<int64_t> &keys) {
  std::string list = "(";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i)
      list += ',';
    list += std::to_string(keys[i]);
  }
  return list + ")";
}

} // namespace scans

class TpchQ3Scan final : public query::TpchQ3InitialScan {
public:
  void ScanOrderLines(
      const std::function<void(const query::TpchQ3OrderLineInput &)> &consumer)
      override {
    scans::ScanTpchQ3OrderLines("", consumer);
  }
  void ScanCustomersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3CustomerInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::CustomersTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeCustomer(cols));
        });
  }
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3OrderInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::OrdersTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeOrder(cols));
        });
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ3LineitemInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::LineitemsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeLineitem(cols));
        });
  }
  bool LookupOrderLines(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ3OrderLineInput &)> &consumer)
      override {
    if (orderkeys.empty())
      return true;
    scans::ScanTpchQ3OrderLines(
        " WHERE o.o_orderkey IN " + scans::KeyList(orderkeys), consumer);
    return true;
  }
  bool LookupOrdersOfCustomers(const std::vector<int64_t> &custkeys,
                               std::vector<int64_t> &orderkeys) override {
    if (custkeys.empty())
      return true;
    ScanTypedTuples("SELECT o_orderkey FROM orders WHERE o_custkey IN " +
                        scans::KeyList(custkeys),
                    [&](const std::vector<std::string> &cols) {
                      orderkeys.push_back(TypedInt(cols, 0));
                    });
    return true;
  }
};

Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan> &Q3Runtime() {
  static Runtime<query::TpchQ3Module, query::TpchQ3Config, TpchQ3Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q3_COST_MODEL",
              core::kTpchQ3CostModel,
              query::TpchQ3Layout::kAccelerator,
              query::TpchQ3Config{mysql_tbl::kCustomer, mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q3Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ3Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q3_aux", "sd", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
