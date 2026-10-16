// MySQL plugin runtime for TPC-H Q17: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q17/mysql/aux/module.hpp"
#include "tpch/q17/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsTable() {
  return {"p_partkey, p_brand, p_container", "part", "", "p_partkey"};
}

query::TpchQ17PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2)};
}

// Rows with a NULL partkey or quantity never take part in the query.
SplitTable LineitemsTable() {
  return {"l_partkey, CAST(ROUND(l_quantity * 100) AS SIGNED), "
          "CAST(ROUND(IFNULL(l_extendedprice, 0) * 100) AS SIGNED)",
          "lineitem", "l_partkey IS NOT NULL AND l_quantity IS NOT NULL", "l_orderkey"};
}

query::TpchQ17LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2)};
}

void ScanTpchQ17Parts(
    const std::function<void(const query::TpchQ17PartInput &)> &consumer) {
  ScanTypedTuplesDecoded(SelectSql(PartsTable()), DecodePart,
                         [&](query::TpchQ17PartInput &&input) { consumer(input); });
}

void ScanTpchQ17Lineitems(
    const std::function<void(const query::TpchQ17LineitemInput &)> &consumer) {
  ScanTypedTuplesDecoded(SelectSql(LineitemsTable()), DecodeLineitem,
                         [&](query::TpchQ17LineitemInput &&input) { consumer(input); });
}

// GROUP BY (partkey, quantity), ordered by partkey so the module can assemble
// one part's histogram at a time. Rows with a NULL partkey or quantity never
// take part in the query.
void ScanTpchQ17PartQuantityAggregates(
    const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
        &consumer) {
  ScanTypedTuples(
      "SELECT l_partkey, CAST(ROUND(l_quantity * 100) AS SIGNED) AS q, "
      "CAST(SUM(CAST(ROUND(IFNULL(l_extendedprice, 0) * 100) AS SIGNED)) "
      "AS SIGNED), CAST(COUNT(*) AS SIGNED) "
      "FROM lineitem WHERE l_partkey IS NOT NULL AND l_quantity IS NOT NULL "
      "GROUP BY l_partkey, q ORDER BY l_partkey, q",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0), TypedInt(cols, 1), TypedInt(cols, 2),
                  TypedInt(cols, 3)});
      });
}

} // namespace scans

class TpchQ17Scan final : public query::TpchQ17InitialScan {
public:
  void ScanParts(
      const std::function<void(const query::TpchQ17PartInput &)> &consumer)
      override {
    scans::ScanTpchQ17Parts(consumer);
  }
  void ScanLineitems(
      const std::function<void(const query::TpchQ17LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ17Lineitems(consumer);
  }
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ17PartInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::PartsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodePart(cols));
        });
  }
  void ScanLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ17LineitemInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::LineitemsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeLineitem(cols));
        });
  }
  bool ScanPartQuantityAggregates(
      const std::function<void(const query::TpchQ17PartQuantityAggregate &)>
          &consumer) override {
    scans::ScanTpchQ17PartQuantityAggregates(consumer);
    return true;
  }
};

Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan> &
Q17Runtime() {
  static Runtime<query::TpchQ17Module, query::TpchQ17Config, TpchQ17Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q17_COST_MODEL",
              core::kTpchQ17CostModel,
              query::TpchQ17Layout::kAccelerator,
              query::TpchQ17Config{mysql_tbl::kPart, mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q17Runtime().Execute(arguments, out);
  if (out.find_first_not_of(" \t\r\n") == std::string::npos) {
    out = "NULL\n";
  }
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ17Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q17_aux", "ss", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
