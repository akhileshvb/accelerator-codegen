// MySQL plugin runtime for TPC-H Q18: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/mysql/mysql_tpch_table_ids.hpp"
#include "tpch/q18/mysql/aux/module.hpp"

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

bool LookupTpchQ18Order(int64_t orderkey, query::TpchQ18OrderInput &out) {
  bool found = false;
  const std::string sql =
      "SELECT o_orderkey, o_custkey, o_orderdate, o_totalprice "
      "FROM orders WHERE o_orderkey = " +
      std::to_string(orderkey);
  ScanTypedTuples(sql, [&](const std::vector<std::string> &cols) {
    out = {TypedInt(cols, 0), TypedInt(cols, 1), TypedDate(cols, 2),
           TypedDecimalRaw(cols, 3)};
    found = out.key == orderkey;
  });
  return found;
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
  bool LookupOrder(int64_t orderkey,
                   query::TpchQ18OrderInput &out) override {
    // SPI is unsafe inside PostgreSQL xact callbacks (PreCommit maintenance).
    // Returning false makes StageMaintenance fall back to RebuildAll.
    (void)orderkey;
    (void)out;
    return false;
  }
};

Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan> &
Q18Runtime() {
  static Runtime<query::TpchQ18Module, query::TpchQ18Config, TpchQ18Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q18_COST_MODEL",
              "config/q18_aux_cost_model.json",
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
    "mysql_accel_tpch_q18_aux", "i", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
