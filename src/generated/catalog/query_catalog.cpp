#include "accelerator_codegen/stateful/core/query_layouts.hpp"
#include "catalog/query_catalog.hpp"
#include "tpch/q17/aux/module.hpp"
#include "tpch/q18/aux/module.hpp"
#include "tpch/q21/aux/module.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace query = accelerator_codegen::aux_state::query;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int64_t DecimalRaw(const ::duckdb::Value &value) {
  return value.IsNull()
             ? 0
             : static_cast<int64_t>(std::llround(std::stod(value.ToString()) *
                                                  100.0));
}

core::AuxValue Text(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::String);
  }
  return core::AuxValue::Of(core::AuxType::String, value.ToString());
}

int64_t SafeInt64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
}

core::AuxValue Int(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::Int64);
  }
  return core::AuxValue::Of(core::AuxType::Int64, SafeInt64(value));
}

std::string SqlLiteral(const std::string &value) {
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

core::AuxValue Number(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return core::AuxValue::Null(core::AuxType::Double);
  }
  return core::AuxValue::Of(core::AuxType::Double,
                            std::stod(value.ToString()));
}

core::AuxValue Date(int32_t days) {
  return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days});
}

core::AuxValue Decimal(int64_t raw, int32_t scale) {
  return core::AuxValue::Of(core::AuxType::ScaledDecimal,
                            core::ScaledDecimal{raw, scale});
}

int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  const auto date = value.GetValue<::duckdb::date_t>();
  return ::duckdb::Date::EpochDays(date);
}

SharedProbe BuildTpchQ1Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(4, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(2, row))),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(6, row)), 2),
                    Text(result.GetValue(7, row)), Text(result.GetValue(8, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(4, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ3Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 3;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Date(DateDays(result.GetValue(4, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ6Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(5, row))),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ10Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 4;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Text(result.GetValue(4, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ12Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)), Text(result.GetValue(6, row)),
                    Date(DateDays(result.GetValue(3, row))),
                    Date(DateDays(result.GetValue(4, row))),
                    Date(DateDays(result.GetValue(5, row)))}};
  probe.new_row = probe.old_row;
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ14Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(5, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Date(DateDays(result.GetValue(4, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ18Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(2, row));
  SharedProbe probe;
  probe.table = 0;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 0.01;
  probe.current_value = static_cast<double>(quantity_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Decimal(quantity_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(quantity_raw + 1, 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 0.01 WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(static_cast<double>(quantity_raw) / 100.0 + 0.01) +
                      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ17Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t price_raw = DecimalRaw(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 0.01;
  probe.current_value = static_cast<double>(quantity_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(2, row)), Decimal(quantity_raw, 2),
                    Decimal(price_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(quantity_raw + 1, 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 0.01 WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(static_cast<double>(quantity_raw) / 100.0 + 0.01) +
                      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ21Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int32_t commit_days = DateDays(result.GetValue(3, row));
  const int32_t receipt_days = DateDays(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = 3;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(receipt_days);
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(2, row)),
                    Date(commit_days), Date(receipt_days)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Date(receipt_days + 1);
  probe.update_sql = "UPDATE lineitem SET l_receiptdate = l_receiptdate + INTERVAL '1' day"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE '" +
                      ::duckdb::Date::ToString(::duckdb::date_t{receipt_days}) +
                      "' WHERE l_orderkey = " + orderkey + " AND l_linenumber = " +
                      linenumber;
  return probe;
}

SharedProbe BuildSecEdgarQ1Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const auto line = result.GetValue(1, row).ToString();
  const auto stmt = result.GetValue(2, row).ToString();
  const auto rfile = result.GetValue(3, row).ToString();
  const int64_t original = SafeInt64(result.GetValue(1, row));
  const auto stmt_sql = SqlLiteral(stmt);
  const auto rfile_sql = SqlLiteral(rfile);
  const auto adsh_sql = SqlLiteral(adsh);
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(original);
  probe.old_row = {{Text(result.GetValue(2, row)), Text(result.GetValue(3, row)),
                    Text(result.GetValue(0, row)), Int(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Int(original + 1);
  probe.update_sql = "UPDATE pre SET line = line + 1 WHERE adsh = " + adsh_sql +
                     " AND line = " + line + " AND stmt = " + stmt_sql +
                     " AND rfile = " + rfile_sql;
  probe.restore_sql = "UPDATE pre SET line = " + std::to_string(original) +
                      " WHERE adsh = " + adsh_sql + " AND line = " +
                      std::to_string(original + 1) + " AND stmt = " + stmt_sql +
                      " AND rfile = " + rfile_sql;
  return probe;
}

SharedProbe BuildSecEdgarQ2Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const auto tag = result.GetValue(1, row).ToString();
  const double value = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.001;
  probe.current_value = value;
  probe.old_row = {{Text(result.GetValue(0, row)), Text(result.GetValue(1, row)),
                    Text(result.GetValue(3, row)), Number(result.GetValue(2, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] =
      core::AuxValue::Of(core::AuxType::Double, value + 0.001);
  probe.update_sql = "UPDATE num SET value = value + 0.001 WHERE adsh = '" +
                     adsh + "' AND tag = '" + tag + "' AND uom = 'pure'";
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE adsh = '" + adsh + "' AND tag = '" + tag +
                      "' AND uom = 'pure'";
  return probe;
}

SharedProbe BuildSecEdgarQ3Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const double value = std::stod(result.GetValue(1, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.001;
  probe.current_value = value;
  probe.old_row = {{Text(result.GetValue(0, row)), Text(result.GetValue(2, row)),
                    Number(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] =
      core::AuxValue::Of(core::AuxType::Double, value + 0.001);
  probe.update_sql = "UPDATE num SET value = value + 0.001 WHERE adsh = '" +
                     adsh + "' AND uom = 'USD'";
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE adsh = '" + adsh + "' AND uom = 'USD'";
  return probe;
}

SharedProbe BuildSecEdgarQ4Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const auto tag = result.GetValue(1, row).ToString();
  const auto version = result.GetValue(2, row).ToString();
  const double value = std::stod(result.GetValue(3, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.001;
  probe.current_value = value;
  probe.old_row = {{Text(result.GetValue(0, row)), Text(result.GetValue(1, row)),
                    Text(result.GetValue(2, row)), Text(result.GetValue(4, row)),
                    Number(result.GetValue(3, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[4] =
      core::AuxValue::Of(core::AuxType::Double, value + 0.001);
  probe.update_sql = "UPDATE num SET value = value + 0.001 WHERE adsh = '" +
                     adsh + "' AND tag = '" + tag + "' AND version = '" +
                     version + "'";
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE adsh = '" + adsh + "' AND tag = '" + tag +
                      "' AND version = '" + version + "'";
  return probe;
}

SharedProbe BuildSecEdgarQ6Probe(::duckdb::MaterializedQueryResult &result,
                                 ::duckdb::idx_t row) {
  const auto adsh = result.GetValue(0, row).ToString();
  const auto tag = result.GetValue(1, row).ToString();
  const auto version = result.GetValue(2, row).ToString();
  const double value = std::stod(result.GetValue(3, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.001;
  probe.current_value = value;
  probe.old_row = {{Text(result.GetValue(0, row)), Text(result.GetValue(1, row)),
                    Text(result.GetValue(2, row)), Text(result.GetValue(4, row)),
                    Number(result.GetValue(3, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[4] =
      core::AuxValue::Of(core::AuxType::Double, value + 0.001);
  probe.update_sql = "UPDATE num SET value = value + 0.001 WHERE adsh = '" +
                     adsh + "' AND tag = '" + tag + "' AND version = '" +
                     version + "'";
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE adsh = '" + adsh + "' AND tag = '" + tag +
                      "' AND version = '" + version + "'";
  return probe;
}

SharedProbe BuildSecEdgarQ24Probe(::duckdb::MaterializedQueryResult &result,
                                  ::duckdb::idx_t row) {
  const auto rowid = result.GetValue(0, row).ToString();
  const double value = std::stod(result.GetValue(5, row).ToString());
  SharedProbe probe;
  probe.table = 101;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.001;
  probe.current_value = value;
  probe.old_row = {{Text(result.GetValue(1, row)), Text(result.GetValue(2, row)),
                    Text(result.GetValue(3, row)), Text(result.GetValue(4, row)),
                    Number(result.GetValue(5, row)), Int(result.GetValue(6, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[4] =
      core::AuxValue::Of(core::AuxType::Double, value + 0.001);
  probe.update_sql = "UPDATE num SET value = value + 0.001 WHERE rowid = " + rowid;
  probe.restore_sql = "UPDATE num SET value = " + std::to_string(value) +
                      " WHERE rowid = " + rowid;
  return probe;
}

const SharedQueryCatalogEntry kTpchCatalog[] = {
    {1,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ1Layout::kAccelerator,
     "SELECT l_returnflag, l_linestatus, "
     "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
     "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
     "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
     "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
     "avg(l_discount) AS avg_disc, count(*) AS count_order "
     "FROM lineitem "
     "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
     "GROUP BY l_returnflag, l_linestatus "
     "ORDER BY l_returnflag, l_linestatus",
     "SELECT * FROM accel_tpch_q1_aux(90)",
     "SELECT * FROM accel_tpch_q1(90)",
     "SELECT * FROM accel_tpch_q1_aux(90)",
     "SELECT l_orderkey, l_linenumber, l_shipdate, l_quantity, l_extendedprice, "
     "l_discount, l_tax, l_returnflag, l_linestatus FROM lineitem "
     "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, int64_t{90})}},
     1,
     false,
     BuildTpchQ1Probe},
    {3,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ3Layout::kAccelerator,
     "SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM ("
     "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
     "o_orderdate, o_shippriority "
     "FROM customer, orders, lineitem "
     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
     "l_orderkey = o_orderkey "
     "AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority "
     "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)",
     "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
     "SELECT * FROM accel_tpch_q3('BUILDING', DATE '1995-03-15')",
     "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate "
     "FROM lineitem WHERE l_returnflag = 'R' AND l_orderkey IN ("
     "SELECT o_orderkey FROM customer JOIN orders ON c_custkey = o_custkey "
     "WHERE c_mktsegment = 'BUILDING' AND o_orderdate < DATE '1995-03-15') "
     "AND l_shipdate > DATE '1995-03-15' ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"BUILDING"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-03-15"))})}},
     3,
     false,
     BuildTpchQ3Probe},
    {6,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ6Layout::kAccelerator,
     "SELECT SUM(l_extendedprice * l_discount) AS revenue "
     "FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
     "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25",
     "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
     "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)",
     "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_quantity, "
     "l_shipdate FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01' "
     "AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25 "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))}),
       core::AuxValue::Of(core::AuxType::Double, 0.03),
       core::AuxValue::Of(core::AuxType::Double, 25.0)}},
     1,
     false,
     BuildTpchQ6Probe},
    {10,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ10Layout::kAccelerator,
     "SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS "
     "revenue, c_acctbal, n_name, c_address, c_phone, c_comment "
     "FROM customer JOIN orders ON c_custkey = o_custkey "
     "JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON "
     "c_nationkey = n_nationkey "
     "WHERE o_orderdate >= DATE '1993-06-01' "
     "AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH AND "
     "l_returnflag = 'R' "
     "GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, "
     "c_comment ORDER BY revenue DESC, c_custkey",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, "
     "l_returnflag FROM lineitem WHERE l_returnflag = 'R' LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1993-06-01"))})}},
     4,
     false,
     BuildTpchQ10Probe},
    {12,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ12Layout::kAccelerator,
     "SELECT l_shipmode, "
     "SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' "
     "THEN 1 ELSE 0 END) AS high_line_count, "
     "SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> "
     "'2-HIGH' THEN 1 ELSE 0 END) AS low_line_count "
     "FROM orders, lineitem "
     "WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate "
     "AND l_receiptdate >= DATE '1995-01-01' "
     "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
     "GROUP BY l_shipmode ORDER BY l_shipmode",
     "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_commitdate, "
     "l_receiptdate, l_shipdate, l_shipmode FROM lineitem "
     "WHERE l_shipmode IN ('MAIL', 'SHIP') "
     "AND l_receiptdate >= DATE '1995-01-01' "
     "AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"MAIL"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"SHIP"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))})}},
     2,
     false,
     BuildTpchQ12Probe},
    {14,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ14Layout::kAccelerator,
     "SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * "
     "(1 - l_discount) ELSE 0 END) / "
     "SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue "
     "FROM lineitem, part "
     "WHERE l_partkey = p_partkey "
     "AND l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + "
     "INTERVAL '1' month",
     "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
     "SELECT * FROM accel_tpch_q14(DATE '1995-06-01')",
     "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, "
     "l_partkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-06-01' + "
     "INTERVAL '1' month ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-06-01"))})}},
     2,
     false,
     BuildTpchQ14Probe},
    {18,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ18Layout::kAccelerator,
     "SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, "
     "SUM(l_quantity) AS sum_qty "
     "FROM customer, orders, lineitem "
     "WHERE o_orderkey IN ("
     "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
     "HAVING SUM(l_quantity) > 312) "
     "AND c_custkey = o_custkey AND o_orderkey = l_orderkey "
     "GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice "
     "ORDER BY o_totalprice DESC, o_orderdate ASC",
     "SELECT * FROM accel_tpch_q18_aux(312)",
     "SELECT * FROM accel_tpch_q18(312)",
     "SELECT * FROM accel_tpch_q18_aux(312)",
     "SELECT l_orderkey, l_linenumber, l_quantity FROM lineitem "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, int64_t{312})}},
     0,
     false,
     BuildTpchQ18Probe},
    {17,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ17Layout::kAccelerator,
     "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#34' AND p_container = 'LARGE BRUSHED' "
     "AND l_quantity < avg_quantity",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LARGE BRUSHED')",
     "SELECT * FROM accel_tpch_q17('Brand#34', 'LARGE BRUSHED')",
     "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LARGE BRUSHED')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_quantity, l_extendedprice "
     "FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#34"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"LARGE BRUSHED"})}},
     2,
     false,
     BuildTpchQ17Probe},
    {21,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ21Layout::kAccelerator,
     "SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, "
     "nation WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
     "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
     "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
     "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
     "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
     "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
     "n_name = 'SAUDI ARABIA' GROUP BY s_name ORDER BY numwait DESC, s_name",
     "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
     "SELECT * FROM accel_tpch_q21('SAUDI ARABIA')",
     "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
     "SELECT l_orderkey, l_linenumber, l_suppkey, l_commitdate, l_receiptdate "
     "FROM lineitem WHERE l_receiptdate > l_commitdate "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"SAUDI ARABIA"})}},
     3,
     false,
     BuildTpchQ21Probe},
};

const SharedQueryCatalogEntry kSecEdgarCatalog[] = {
    {1,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ1Layout::kAccelerator,
     "SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings, "
     "AVG(line) AS avg_line_num "
     "FROM pre WHERE stmt IS NOT NULL GROUP BY stmt, rfile ORDER BY cnt DESC",
     "SELECT * FROM accel_sec_edgar_q1_aux()",
     "SELECT * FROM accel_sec_edgar_q1()",
     "SELECT * FROM accel_sec_edgar_q1_aux()",
     "SELECT adsh, line, stmt, rfile FROM pre WHERE stmt IS NOT NULL "
     "ORDER BY adsh, line LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ1Probe},
    {2,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ2Layout::kAccelerator,
     "SELECT s.name, n.tag, n.value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN ("
     "    SELECT adsh, tag, MAX(value) AS max_value "
     "    FROM num "
     "    WHERE uom = 'pure' AND value IS NOT NULL "
     "    GROUP BY adsh, tag"
     ") m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
     "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.value DESC, s.name, n.tag "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q2_aux()",
     "SELECT * FROM accel_sec_edgar_q2()",
     "SELECT * FROM accel_sec_edgar_q2_aux()",
     "SELECT n.adsh, n.tag, n.value, n.uom FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.adsh, n.tag, n.value DESC LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ2Probe},
    {3,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ3Layout::kAccelerator,
     "SELECT s.name, s.cik, SUM(n.value) AS total_value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
     "GROUP BY s.name, s.cik "
     "HAVING SUM(n.value) > ("
     "    SELECT AVG(sub_total) FROM ("
     "        SELECT SUM(n2.value) AS sub_total "
     "        FROM num n2 "
     "        JOIN sub s2 ON n2.adsh = s2.adsh "
     "        WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
     "        GROUP BY s2.cik"
     "    ) avg_sub"
     ") "
     "ORDER BY total_value DESC "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q3_aux()",
     "SELECT * FROM accel_sec_edgar_q3()",
     "SELECT * FROM accel_sec_edgar_q3_aux()",
     "SELECT n.adsh, n.value, n.uom FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
     "ORDER BY n.adsh LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ3Probe},
    {4,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ4Layout::kAccelerator,
     "SELECT s.sic, t.tlabel, p.stmt, "
     "COUNT(DISTINCT s.cik) AS num_companies, "
     "SUM(n.value) AS total_value, "
     "AVG(n.value) AS avg_value "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'EQ' "
     "AND s.sic BETWEEN 4000 AND 4999 "
     "AND n.value IS NOT NULL AND t.abstract = 0 "
     "GROUP BY s.sic, t.tlabel, p.stmt "
     "HAVING COUNT(DISTINCT s.cik) >= 2 "
     "ORDER BY total_value DESC "
     "LIMIT 500",
     "SELECT * FROM accel_sec_edgar_q4_aux()",
     "SELECT * FROM accel_sec_edgar_q4()",
     "SELECT * FROM accel_sec_edgar_q4_aux()",
     "SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
     "AND n.value IS NOT NULL AND t.abstract = 0 "
     "ORDER BY n.adsh, n.tag, n.version LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ4Probe},
    {6,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ6Layout::kAccelerator,
     "SELECT s.name, p.stmt, n.tag, p.plabel, "
     "SUM(n.value) AS total_value, COUNT(*) AS cnt "
     "FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
     "AND n.value IS NOT NULL "
     "GROUP BY s.name, p.stmt, n.tag, p.plabel "
     "ORDER BY total_value DESC, s.name, n.tag, p.plabel "
     "LIMIT 200",
     "SELECT * FROM accel_sec_edgar_q6_aux()",
     "SELECT * FROM accel_sec_edgar_q6()",
     "SELECT * FROM accel_sec_edgar_q6_aux()",
     "SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n "
     "JOIN sub s ON n.adsh = s.adsh "
     "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
     "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
     "AND n.value IS NOT NULL "
     "ORDER BY n.adsh, n.tag, n.version LIMIT 1",
     {},
     1,
     false,
     BuildSecEdgarQ6Probe},
    {24,
     SharedWorkloadKind::SecEdgar,
     "sec_edgar",
     core::SecEdgarQ24Layout::kAccelerator,
     "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
     "FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh "
     "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
     "AND n.value IS NOT NULL AND p.adsh IS NULL "
     "GROUP BY n.tag, n.version "
     "HAVING COUNT(*) > 10 "
     "ORDER BY cnt DESC "
     "LIMIT 100",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT * FROM accel_sec_edgar_q24()",
     "SELECT * FROM accel_sec_edgar_q24_aux()",
     "SELECT n.rowid, n.adsh, n.tag, n.version, n.uom, n.value, n.ddate FROM num n "
     "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
     "n.adsh = p.adsh WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 "
     "AND 20231231 AND n.value IS NOT NULL AND p.adsh IS NULL LIMIT 1",
     {},
     101,
     true,
     BuildSecEdgarQ24Probe},
};

const SharedQueryCatalogEntry *FindEntry(SharedWorkloadKind workload,
                                        int query_id) {
  if (workload == SharedWorkloadKind::Tpch) {
    for (const auto &entry : kTpchCatalog) {
      if (entry.query_id == query_id) {
        return &entry;
      }
    }
  } else {
    for (const auto &entry : kSecEdgarCatalog) {
      if (entry.query_id == query_id) {
        return &entry;
      }
    }
  }
  return nullptr;
}

SharedWorkloadKind ParseWorkload(const std::string &workload_name) {
  if (workload_name == "tpch") {
    return SharedWorkloadKind::Tpch;
  }
  if (workload_name == "sec_edgar") {
    return SharedWorkloadKind::SecEdgar;
  }
  throw std::runtime_error("unsupported workload: " + workload_name);
}

} // namespace

core::QueryArguments ToCoreArguments(const SharedQueryArguments &arguments) {
  return core::QueryArguments{arguments.values};
}

const SharedQueryCatalogEntry &LookupSharedQuery(SharedWorkloadKind workload,
                                               int query_id) {
  const auto *entry = FindEntry(workload, query_id);
  Require(entry != nullptr, "unsupported shared query");
  return *entry;
}

const SharedQueryCatalogEntry &LookupSharedQuery(const std::string &workload_name,
                                               int query_id) {
  return LookupSharedQuery(ParseWorkload(workload_name), query_id);
}

std::vector<int> SharedQueriesForWorkload(SharedWorkloadKind workload) {
  if (workload == SharedWorkloadKind::Tpch) {
    return {1, 3, 6, 10, 12, 14, 17, 18, 21};
  }
  return {1, 2, 3, 4, 6, 24};
}

std::vector<int> SharedQueriesForWorkload(const std::string &workload_name) {
  return SharedQueriesForWorkload(ParseWorkload(workload_name));
}

SharedProbe FindSharedProbe(::duckdb::Connection &connection,
                            const SharedQueryCatalogEntry &entry) {
  auto result = connection.Query(entry.probe_select_sql);
  Require(result != nullptr && !result->HasError() && result->RowCount() >= 1,
          result ? ("shared probe failed for Q" + std::to_string(entry.query_id) +
                      ": " + result->GetError())
                 : "shared probe returned null");
  auto &materialized = result->Cast<::duckdb::MaterializedQueryResult>();
  return entry.build_probe(materialized, 0);
}

core::MutationBatch BuildSharedMutation(const SharedQueryCatalogEntry &entry,
                                        const SharedProbe &probe) {
  core::MutationBatch mutation;
  mutation.table = probe.table;
  mutation.kind = probe.kind;
  mutation.updated_columns = probe.updated_columns;
  mutation.old_rows = {probe.old_row};
  mutation.new_rows = {probe.new_row};
  (void)entry;
  return mutation;
}

} // namespace accelerator_codegen::aux_state::engine
