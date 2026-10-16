// MySQL plugin runtime for TPC-H Q19: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include <stdexcept>

#include "prims/decimal_text.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q19/mysql/aux/module.hpp"
#include "tpch/q19/cost_model.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

SplitTable PartsTable() {
  return {"p_partkey, p_brand, p_container, p_size", "part", "", "p_partkey"};
}

query::TpchQ19PartInput DecodePart(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedText(cols, 1), TypedText(cols, 2),
          static_cast<int32_t>(TypedInt(cols, 3))};
}

// The lineitems of the shipping predicate (comparisons follow the columns'
// collation); revenue is l_extendedprice * (1 - l_discount) at scale 4.
SplitTable ShippedLineitemsTable() {
  return {"l_partkey, l_quantity, IFNULL(l_extendedprice * (1 - l_discount), 0)", "lineitem",
          query::TpchQ19ShippingSql(), "l_orderkey"};
}

query::TpchQ19LineitemInput DecodeLineitem(const std::vector<std::string> &cols) {
  return {TypedInt(cols, 0), TypedDecimalRaw(cols, 1), TypedDecimal(cols, 2, 4)};
}

// Point re-aggregation of the given parts' qualifying lineitems (the schema
// has no l_partkey index: this is a table scan on MySQL).
void ScanTpchQ19PartQuantityRevenue(
    const std::vector<int64_t> &partkeys,
    const std::function<void(const query::TpchQ19PartQuantityRevenue &)>
        &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  if (partkeys.empty())
    return;
  std::string keys;
  for (const auto key : partkeys)
    keys += (keys.empty() ? "" : ",") + std::to_string(key);
  ScanTypedTuples(
      "SELECT l_partkey, l_quantity, sum(l_extendedprice * (1 - l_discount)), "
      "count(*) FROM lineitem WHERE l_partkey IN (" + keys + ") AND " +
          query::TpchQ19ShippingSql() + " GROUP BY l_partkey, l_quantity",
      [&](const std::vector<std::string> &cols) {
        query::TpchQ19PartQuantityRevenue row;
        row.partkey = TypedInt(cols, 0);
        __int128 quantity = 0;
        if (!dec::Parse(TypedText(cols, 1), 2, quantity) ||
            !dec::Parse(TypedText(cols, 2), 4, row.revenue_raw))
          throw std::runtime_error("TPC-H Q19: unparsable decimal");
        row.quantity_raw = static_cast<int64_t>(quantity);
        row.count = TypedInt(cols, 3);
        consumer(row);
      });
}

} // namespace scans

class TpchQ19Scan final : public query::TpchQ19InitialScan {
public:
  void ScanPartsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ19PartInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::PartsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodePart(cols));
        });
  }
  void ScanShippedLineitemsPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ19LineitemInput &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::ShippedLineitemsTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeLineitem(cols));
        });
  }
  void ScanPartQuantityRevenue(
      const std::vector<int64_t> &partkeys,
      const std::function<void(const query::TpchQ19PartQuantityRevenue &)>
          &consumer) override {
    scans::ScanTpchQ19PartQuantityRevenue(partkeys, consumer);
  }
};

Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan> &
Q19Runtime() {
  static Runtime<query::TpchQ19Module, query::TpchQ19Config, TpchQ19Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q19_COST_MODEL",
              core::kTpchQ19CostModel,
              query::TpchQ19Layout::kAccelerator,
              query::TpchQ19Config{mysql_tbl::kLineitem, mysql_tbl::kPart});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q19Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ19Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q19_aux", "sisisi", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
