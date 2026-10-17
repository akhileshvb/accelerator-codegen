// MySQL plugin runtime for TPC-H Q9: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_split_scan.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q9/mysql/aux/module.hpp"
#include "tpch/q9/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsTable() {
  return {"p_partkey, p_name", "part", "p_partkey IS NOT NULL AND p_name IS NOT NULL",
          "p_partkey"};
}

query::TpchQ9PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1)};
}

SplitTable SuppliersTable() {
  return {"s_suppkey, s_nationkey", "supplier",
          "s_suppkey IS NOT NULL AND s_nationkey IS NOT NULL", "s_suppkey"};
}

query::TpchQ9SupplierInput DecodeSupplier(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1))};
}

void ScanTpchQ9NationsSerial(
    const std::function<void(const query::TpchQ9NationInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT n_nationkey, n_name FROM nation "
      "WHERE n_nationkey IS NOT NULL AND n_name IS NOT NULL",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ9NationInput{static_cast<int32_t>(TypedInt(cols, 0)),
                                        TypedText(cols, 1)};
      },
      [&](query::TpchQ9NationInput &&input) { consumer(input); });
}

SplitTable OrdersTable() {
  return {"o_orderkey, o_orderdate", "orders",
          "o_orderkey IS NOT NULL AND o_orderdate IS NOT NULL", "o_orderkey"};
}

query::TpchQ9OrdersInput DecodeOrder(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDate(cols, 1)};
}

SplitTable PartsuppsTable() {
  return {"ps_partkey, ps_suppkey, ps_supplycost", "partsupp",
          "ps_partkey IS NOT NULL AND ps_suppkey IS NOT NULL AND "
          "ps_supplycost IS NOT NULL",
          "ps_partkey"};
}

query::TpchQ9PartsuppInput DecodePartsupp(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedDecimalRaw(cols, 2)};
}

void ScanTpchQ9LineitemsSerial(
    const std::function<void(const query::TpchQ9LineitemInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT l_partkey, l_suppkey, l_orderkey, l_quantity, l_extendedprice, "
      "l_discount FROM lineitem "
      "WHERE l_partkey IS NOT NULL AND l_suppkey IS NOT NULL AND "
      "l_orderkey IS NOT NULL AND l_quantity IS NOT NULL AND "
      "l_extendedprice IS NOT NULL AND l_discount IS NOT NULL",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ9LineitemInput{TypedInt(cols, 0),
                                          TypedInt(cols, 1),
                                          TypedInt(cols, 2),
                                          TypedDecimalRaw(cols, 3),
                                          TypedDecimalRaw(cols, 4),
                                          TypedDecimalRaw(cols, 5)};
      },
      [&](query::TpchQ9LineitemInput &&input) { consumer(input); });
}

// Pre-aggregation in SQL: the DBMS runs the join lineitem x partsupp x supplier
// x orders and groups it by (part, supplier nation, order year); the module
// keeps those groups as its profit cells. Amounts are in 1e-4 units. Split by
// l_partkey, each part's groups come from one partition.
SplitTable ProfitContributionsTable() {
  return {"l_partkey, s_nationkey, YEAR(o_orderdate), COUNT(*), "
          "CAST(SUM(l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity) "
          "* 10000 AS SIGNED)",
          "lineitem, partsupp, supplier, orders",
          "ps_suppkey = l_suppkey AND ps_partkey = l_partkey "
          "AND s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
          "AND l_extendedprice IS NOT NULL AND l_discount IS NOT NULL "
          "AND l_quantity IS NOT NULL AND ps_supplycost IS NOT NULL",
          "l_partkey", "l_partkey, s_nationkey, YEAR(o_orderdate)", "lineitem"};
}

query::TpchQ9ProfitContribution DecodeProfitContribution(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), static_cast<int32_t>(TypedInt(cols, 1)),
          static_cast<int32_t>(TypedInt(cols, 2)), TypedInt(cols, 3), TypedInt(cols, 4)};
}

} // namespace scans

class TpchQ9Scan final : public query::TpchQ9InitialScan {
public:
  void ScanNationsSerial(
      const std::function<void(const query::TpchQ9NationInput &)> &consumer)
      override {
    scans::ScanTpchQ9NationsSerial(consumer);
  }
  void ScanLineitemsSerial(
      const std::function<void(const query::TpchQ9LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ9LineitemsSerial(consumer);
  }
  void ScanParts(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9PartInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::PartsTable(), partitions, scans::DecodePart, sink);
  }
  void ScanSuppliers(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9SupplierInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::SuppliersTable(), partitions,
                            scans::DecodeSupplier, sink);
  }
  void ScanOrders(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9OrdersInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::OrdersTable(), partitions, scans::DecodeOrder,
                            sink);
  }
  void ScanPartsupps(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9PartsuppInput &)> &sink)
      override {
    split_scan::Partitioned(services, scans::PartsuppsTable(), partitions,
                            scans::DecodePartsupp, sink);
  }
  bool ScanProfitContributions(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ9ProfitContribution &)> &sink)
      override {
    split_scan::Partitioned(services, scans::ProfitContributionsTable(), partitions,
                            scans::DecodeProfitContribution, sink);
    return true;
  }
};

Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan> &Q9Runtime() {
  static Runtime<query::TpchQ9Module, query::TpchQ9Config, TpchQ9Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q9_COST_MODEL",
              core::kTpchQ9CostModel,
              query::TpchQ9Layout::kAccelerator,
              query::TpchQ9Config{mysql_tbl::kPart, mysql_tbl::kSupplier, mysql_tbl::kNation, mysql_tbl::kOrders, mysql_tbl::kPartsupp, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q9Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ9Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q9_aux", "s", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
