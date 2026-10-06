// MySQL plugin runtime for TPC-H Q19: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// src/generated/mysql/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see src/generated/mysql/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "mysql/runtime.hpp"

#include <stdexcept>

#include "accelerator_codegen/stateful/core/decimal_text.hpp"
#include "tpch/mysql_table_ids.hpp"
#include "tpch/q19/mysql/aux/module.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ19Parts(
    const std::function<void(const query::TpchQ19PartInput &)> &consumer) {
  ScanTypedTuples(
      "SELECT p_partkey, p_brand, p_container, p_size FROM part",
      [&](const std::vector<std::string> &cols) {
        consumer({TypedInt(cols, 0),
                  TypedText(cols, 1),
                  TypedText(cols, 2),
                  static_cast<int32_t>(TypedInt(cols, 3))});
      });
}

// lineitem JOIN part with the shipping predicate and family classification
// applied and grouped in the engine: one row per (brand, family, quantity)
// with exact decimal sums (text). Comparisons follow the columns' collation.
void ScanTpchQ19GroupAggregates(
    const std::function<void(const query::TpchQ19GroupAggregate &)> &consumer) {
  namespace dec = accelerator_codegen::aux_state::core::decimal_text;
  const std::string family = query::TpchQ19FamilySql();
  ScanTypedTuples(
      "SELECT p_brand, " + family +
          " AS fam, l_quantity, sum(l_extendedprice * (1 - l_discount)), "
          "count(*) FROM lineitem, part WHERE l_partkey = p_partkey AND " +
          query::TpchQ19ShippingSql() + " AND " + family +
          " IS NOT NULL GROUP BY p_brand, fam, l_quantity",
      [&](const std::vector<std::string> &cols) {
        query::TpchQ19GroupAggregate agg;
        agg.brand = TypedText(cols, 0);
        agg.family = static_cast<int>(TypedInt(cols, 1));
        __int128 quantity = 0;
        if (!dec::Parse(TypedText(cols, 2), 2, quantity) ||
            !dec::Parse(TypedText(cols, 3), 4, agg.revenue_raw))
          throw std::runtime_error("TPC-H Q19: unparsable decimal");
        agg.quantity_raw = static_cast<int64_t>(quantity);
        agg.count = TypedInt(cols, 4);
        consumer(agg);
      });
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
  void ScanParts(
      const std::function<void(const query::TpchQ19PartInput &)> &consumer)
      override {
    scans::ScanTpchQ19Parts(consumer);
  }
  void ScanGroupAggregates(
      const std::function<void(const query::TpchQ19GroupAggregate &)> &consumer)
      override {
    scans::ScanTpchQ19GroupAggregates(consumer);
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
              "config/q19_aux_cost_model.json",
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
