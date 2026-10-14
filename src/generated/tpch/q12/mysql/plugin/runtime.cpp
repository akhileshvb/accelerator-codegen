// MySQL plugin runtime for TPC-H Q12: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include "tpch/mysql_table_ids.hpp"
#include "tpch/q12/mysql/aux/module.hpp"
#include "tpch/q12/cost_model.hpp"
#include "tpch/q12/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT o_orderkey, o_orderpriority, "
      "CASE WHEN o_orderpriority IS NULL THEN 1 ELSE 0 END "
      "FROM orders WHERE o_orderkey IS NOT NULL",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedInt(cols, 2) != 0});
      });
}

// Lineitems that can contribute: non-NULL key / shipmode, commit < receipt,
// ship < commit (NULL dates fail the comparisons).  `where` narrows further.
void ScanTpchQ12QualifyingLineitems(
    const std::string &where,
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate "
      "FROM lineitem WHERE l_orderkey IS NOT NULL AND l_shipmode IS NOT NULL "
      "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate" +
          where,
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedDate(cols, 2),
                  TypedDate(cols, 3),
                  TypedDate(cols, 4)});
      });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTpchQ12QualifyingLineitems("", consumer);
}

// Index lookup of the qualifying lineitems of some orders.
void LookupTpchQ12LineitemsByOrder(
    const std::vector<int64_t> &orderkeys,
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  constexpr std::size_t kBatch = 4096;
  for (std::size_t from = 0; from < orderkeys.size(); from += kBatch) {
    const std::vector<int64_t> batch(
        orderkeys.begin() + from,
        orderkeys.begin() + std::min(orderkeys.size(), from + kBatch));
    ScanTpchQ12QualifyingLineitems(
        " AND " + KeyListPredicate("l_orderkey", batch), consumer);
  }
}

} // namespace scans

class TpchQ12Scan final : public query::TpchQ12InitialScan {
public:
  void ScanOrders(
      const std::function<void(const query::TpchQ12OrderInput &)> &consumer)
      override {
    scans::ScanTpchQ12Orders(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ12Lineitems(consumer);
  }
  void LookupLineitemsByOrder(
      const std::vector<int64_t> &orderkeys,
      const std::function<void(const query::TpchQ12LineitemInput &)> &consumer)
      override {
    scans::LookupTpchQ12LineitemsByOrder(orderkeys, consumer);
  }
};

Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan> &
Q12Runtime() {
  static Runtime<query::TpchQ12Module, query::TpchQ12Config, TpchQ12Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q12_COST_MODEL",
              core::kTpchQ12CostModel,
              core::TpchQ12Layout::kAccelerator, query::TpchQ12Config{mysql_tbl::kOrders, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q12Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ12Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q12_aux", "ssd", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
