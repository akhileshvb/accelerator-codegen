// MySQL plugin runtime for TPC-H Q1: table scans, scan class, aux runtime and
// the UDF entry point.  Moved verbatim from the former shared
// backends/mysql/api/runtime.cpp; it plugs into the backend through
// MysqlUdfRegistrar (see backends/mysql/api/runtime.hpp).
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/runtime.hpp"

#include <stdexcept>

#include "prims/decimal_text.hpp"
#include "tpch/common/mysql_table_ids.hpp"
#include "tpch/q1/mysql/aux/module.hpp"
#include "tpch/q1/cost_model.hpp"
#include "tpch/q1/layout.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;
namespace mysql_tbl = accelerator_codegen::aux_state::engine::mysql::tpch_tables;
using namespace detail;

namespace scans {

void ScanTpchQ1Lineitems(
    const std::function<void(const query::TpchQ1LineitemInput &)> &consumer) {
  ScanTypedTuplesDecoded(
      "SELECT l_shipdate, l_quantity, l_extendedprice, l_discount, l_tax, "
      "l_returnflag, l_linestatus FROM lineitem",
      [](const std::vector<std::string> &cols) {
        return query::TpchQ1LineitemInput{TypedDate(cols, 0),
                                          TypedDecimalRaw(cols, 1), 2,
                                          TypedDecimalRaw(cols, 2), 2,
                                          TypedDecimalRaw(cols, 3), 2,
                                          TypedDecimalRaw(cols, 4), 2,
                                          TypedChar(cols, 5),
                                          TypedChar(cols, 6)};
      },
      [&](query::TpchQ1LineitemInput &&input) { consumer(input); });
}

// One SQL GROUP BY: the engine aggregates, the process receives one row per
// (shipdate, returnflag, linestatus) cell with exact DECIMAL sums (text).
SplitTable DayGroupAggregatesTable() {
  return {"l_shipdate, l_returnflag, l_linestatus, count(*), "
          "sum(l_quantity), sum(l_extendedprice), sum(l_discount), "
          "sum(l_extendedprice * (1 - l_discount)), "
          "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax))",
          "lineitem", "", "l_orderkey", "l_shipdate, l_returnflag, l_linestatus"};
}

query::TpchQ1DayGroupAggregate DecodeDayGroupAggregate(const std::vector<std::string> &cols) {
  query::TpchQ1DayGroupAggregate agg;
  agg.shipdate_days = TypedDate(cols, 0);
  agg.returnflag = TypedChar(cols, 1);
  agg.linestatus = TypedChar(cols, 2);
  agg.count = TypedInt(cols, 3);
  const auto sum = [&](int attribute, int scale) {
    __int128 value = 0;
    if (!core::decimal_text::Parse(TypedText(cols, attribute), scale, value))
      throw std::runtime_error("TPC-H Q1: unparsable decimal aggregate");
    return value;
  };
  agg.sum_qty_raw = sum(4, 2);
  agg.sum_base_price_raw = sum(5, 2);
  agg.sum_discount_raw = sum(6, 2);
  agg.sum_disc_price_raw = sum(7, 4);
  agg.sum_charge_raw = sum(8, 6);
  return agg;
}

void ScanTpchQ1DayGroupAggregates(
    const std::function<void(const query::TpchQ1DayGroupAggregate &)> &consumer) {
  ScanTypedTuples(SelectSql(DayGroupAggregatesTable()),
                  [&](const std::vector<std::string> &cols) {
                    consumer(DecodeDayGroupAggregate(cols));
                  });
}

} // namespace scans

class TpchQ1Scan final : public query::TpchQ1InitialScan {
public:
  void ScanLineitems(
      const std::function<void(const query::TpchQ1LineitemInput &)> &consumer)
      override {
    scans::ScanTpchQ1Lineitems(consumer);
  }
  void ScanDayGroupAggregates(
      const std::function<void(const query::TpchQ1DayGroupAggregate &)>
          &consumer) override {
    scans::ScanTpchQ1DayGroupAggregates(consumer);
  }
  void ScanDayGroupAggregatesPartitioned(
      const core::AuxEngineServices &services, std::size_t partitions,
      const std::function<void(std::size_t, const query::TpchQ1DayGroupAggregate &)> &sink)
      override {
    ScanTypedTuplesPartitioned(
        services, scans::DayGroupAggregatesTable(), partitions,
        [&](std::size_t partition, const std::vector<std::string> &cols) {
          sink(partition, scans::DecodeDayGroupAggregate(cols));
        });
  }
};

Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan> &Q1Runtime() {
  static Runtime<query::TpchQ1Module, query::TpchQ1Config, TpchQ1Scan>
      runtime("ACCELERATOR_CODEGEN_MYSQL_Q1_COST_MODEL",
              core::kTpchQ1CostModel,
              core::TpchQ1Layout::kAccelerator, query::TpchQ1Config{mysql_tbl::kLineitem});
  return runtime;
}

std::string ExecuteQuery(const core::QueryArguments &arguments) {
  std::string out;
  Q1Runtime().Execute(arguments, out);
  return out;
}

} // namespace

static const MysqlUdfRegistrar kRegisterQ1Udf(MysqlUdfEntry{
    "mysql_accel_tpch_q1_aux", "i", &ExecuteQuery});

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
