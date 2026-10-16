// MySQL plugin runtime for TPC-H Q12: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
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

SplitTable OrdersTable() {
  return {"o_orderkey, o_orderpriority, "
          "CASE WHEN o_orderpriority IS NULL THEN 1 ELSE 0 END",
          "orders", "o_orderkey IS NOT NULL", "o_orderkey"};
}

query::TpchQ12OrderInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedInt(cols, 2) != 0};
}

// Lineitems that can contribute: non-NULL key / shipmode, commit < receipt,
// ship < commit (NULL dates fail the comparisons).
SplitTable QualifyingLineitemsTable() {
  return {"l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate", "lineitem",
          "l_orderkey IS NOT NULL AND l_shipmode IS NOT NULL "
          "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate",
          "l_orderkey"};
}

query::TpchQ12LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedDate(cols, 2), TypedDate(cols, 3),
          TypedDate(cols, 4)};
}

void ScanTpchQ12Orders(
    const std::function<void(const query::TpchQ12OrderInput &)> &consumer) {
  ScanTypedTuplesDecoded(SelectSql(OrdersTable()), DecodeOrder,
                         [&](query::TpchQ12OrderInput &&input) { consumer(input); });
}

void ScanTpchQ12Lineitems(
    const std::function<void(const query::TpchQ12LineitemInput &)> &consumer) {
  ScanTypedTuplesDecoded(SelectSql(QualifyingLineitemsTable()), DecodeLineitem,
                         [&](query::TpchQ12LineitemInput &&input) { consumer(input); });
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
    ScanTypedTuplesDecoded(
        SelectSql(QualifyingLineitemsTable()) + " AND " + KeyListPredicate("l_orderkey", batch),
        DecodeLineitem, [&](query::TpchQ12LineitemInput &&input) { consumer(input); });
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
  void ScanOrdersPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ12OrderInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::OrdersTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeOrder(cols));
        });
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ12LineitemInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::QualifyingLineitemsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeLineitem(cols));
        });
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
