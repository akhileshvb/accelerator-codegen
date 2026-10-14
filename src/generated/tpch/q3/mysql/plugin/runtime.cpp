// MySQL plugin runtime for TPC-H Q3: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "tpch/mysql_table_ids.hpp"
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
  ScanTypedTuples(
      "SELECT o.o_orderkey, o.o_orderdate, o.o_shippriority, c.c_mktsegment, "
      "l.l_shipdate, l.l_extendedprice, l.l_discount "
      "FROM orders o JOIN customer c ON c.c_custkey = o.o_custkey "
      "JOIN lineitem l ON l.l_orderkey = o.o_orderkey" +
          restrict_sql + " ORDER BY o.o_orderkey",
      [&](const std::vector<std::string> &cols) {
        query::TpchQ3OrderLineInput line;
        line.orderkey = TypedInt(cols, 0);
        line.orderdate_days = TypedDate(cols, 1);
        line.shippriority = static_cast<int32_t>(TypedInt(cols, 2));
        line.mktsegment = TypedText(cols, 3);
        line.shipdate_days = TypedDate(cols, 4);
        line.extended_price_raw = TypedDecimalRaw(cols, 5);
        line.discount_raw = TypedDecimalRaw(cols, 6);
        consumer(line);
      });
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
