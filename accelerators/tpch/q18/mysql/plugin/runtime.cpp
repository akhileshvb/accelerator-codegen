// MySQL plugin runtime for TPC-H Q18: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q18/mysql/aux/module.hpp"
#include "tpch/q18/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ18Lineitems(
    const std::function<void(const query::TpchQ18LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_quantity FROM lineitem",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1)});
      });
}

void ScanTpchQ18Orders(
    const std::function<void(const query::TpchQ18OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice FROM orders",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedInt(cols, 1),
                  TypedDate(cols, 2),
                  TypedDecimalRaw(cols, 3)});
      });
}

void ScanTpchQ18Customers(
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT c_custkey, c_name FROM customer",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1)});
      });
}

// SUM(l_quantity) and COUNT(*) per order, computed by the database.
bool ScanTpchQ18OrderQuantityAggregates(
    const std::function<void(const query::TpchQ18OrderQuantityAggregate &)>
        &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, SUM(l_quantity), COUNT(*) FROM lineitem "
      "GROUP BY l_orderkey",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedDecimalRaw(cols, 1),
                  TypedInt(cols, 2)});
      });
  return true;
}

// Keyed lookup of customer names (the primary key answers it).
void LookupTpchQ18CustomerNames(
    const std::vector<int64_t> &custkeys,
    const std::function<void(const query::TpchQ18CustomerInput &)> &consumer) {
  std::string sql = "SELECT c_custkey, c_name FROM customer WHERE c_custkey IN (";
  for (std::size_t i = 0; i < custkeys.size(); ++i) {
    if (i)
      sql += ',';
    sql += std::to_string(custkeys[i]);
  }
  sql += ')';
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    consumer({TypedInt(cols, 0), TypedText(cols, 1)});
  });
}

} // namespace scans

class TpchQ18Scan final : public query::TpchQ18InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ18LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ18Lineitems(consumer);
  }
  void ScanOrders(
      const std::function<void(const query::TpchQ18OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ18Orders(consumer);
  }
  void ScanCustomers(
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    scans::ScanTpchQ18Customers(consumer);
  }
  bool ScanOrderQuantityAggregates(
      const std::function<void(const query::TpchQ18OrderQuantityAggregate &)>
          &consumer) override {
    return scans::ScanTpchQ18OrderQuantityAggregates(consumer);
  }
  void LookupCustomerNames(
      const std::vector<int64_t> &custkeys,
      const std::function<void(const query::TpchQ18CustomerInput &)> &consumer)
      override {
    // A big key list is cheaper as one scan than as an IN list.
    if (custkeys.size() > 4096)
      query::TpchQ18InitialScan::LookupCustomerNames(custkeys, consumer);
    else
      scans::LookupTpchQ18CustomerNames(custkeys, consumer);
  }
};

Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q18_COST_MODEL",
              core::kTpchQ18CostModel,
              query::TpchQ18Layout::kAccelerator,
              query::TpchQ18Config{mysql_tbl::kLineitem, mysql_tbl::kOrders, mysql_tbl::kCustomer});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q18Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ18Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q18_aux", "s", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
