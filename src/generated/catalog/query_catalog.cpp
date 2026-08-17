#include "accelerator_codegen/stateful/core/query_layouts.hpp"
#include "catalog/query_catalog.hpp"
#include "tpch/q5/aux/module.hpp"
#include "tpch/q9/aux/module.hpp"
#include "tpch/q11/aux/module.hpp"
#include "tpch/q16/aux/module.hpp"
#include "tpch/q17/aux/module.hpp"
#include "tpch/q18/aux/module.hpp"
#include "tpch/q21/aux/module.hpp"
#include "tpch/q2/aux/module.hpp"
#include "tpch/q4/aux/module.hpp"
#include "tpch/q7/aux/module.hpp"
#include "tpch/q8/aux/module.hpp"
#include "tpch/q13/aux/module.hpp"
#include "tpch/q15/aux/module.hpp"
#include "tpch/q19/aux/module.hpp"
#include "tpch/q20/aux/module.hpp"
#include "tpch/q22/aux/module.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
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
  if (value.IsNull()) {
    return 0;
  }
  try {
    return static_cast<int64_t>(
        std::llround(std::stod(value.ToString()) * 100.0));
  } catch (const std::exception &) {
    return 0;
  }
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
  try {
    return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
  } catch (const std::exception &) {
    return 0;
  }
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

SharedProbe BuildTpchQ5Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 6;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {5};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
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

SharedProbe BuildTpchQ9Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Aux row layout matches TpchQ9 Dependencies lineitem cols:
  // partkey, suppkey, orderkey, quantity, extendedprice, discount.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(5, row).ToString());
  SharedProbe probe;
  probe.table = 6;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {4};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(2, row)), Int(result.GetValue(3, row)),
                    Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(6, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[4] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(5, row)) + 1), 2);
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

SharedProbe BuildTpchQ11Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Aux row: partkey, suppkey, availqty, supplycost (TpchQ11Config partsupp=1).
  const auto partkey = result.GetValue(0, row).ToString();
  const auto suppkey = result.GetValue(1, row).ToString();
  const double supplycost = std::stod(result.GetValue(3, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 0.01;
  probe.current_value = supplycost;
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(1, row)),
                    Int(result.GetValue(2, row)),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(3, row)) + 1), 2);
  probe.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01"
                     " WHERE ps_partkey = " +
                     partkey + " AND ps_suppkey = " + suppkey;
  probe.restore_sql = "UPDATE partsupp SET ps_supplycost = " +
                      std::to_string(supplycost) + " WHERE ps_partkey = " +
                      partkey + " AND ps_suppkey = " + suppkey;
  return probe;
}

SharedProbe BuildTpchQ12Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Q12 counts by shipmode — price updates never change the answer. Flip MAIL
  // <-> AIR (outside the queried {'MAIL','SHIP'} pair) so high/low counts move.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const std::string shipmode = result.GetValue(6, row).ToString();
  const std::string new_shipmode = (shipmode == "MAIL") ? "AIR" : "MAIL";
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1}; // shipmode slot in aux lineitem row
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Text(result.GetValue(6, row)),
                    Date(DateDays(result.GetValue(3, row))),
                    Date(DateDays(result.GetValue(4, row))),
                    Date(DateDays(result.GetValue(5, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::String, new_shipmode);
  probe.update_sql = "UPDATE lineitem SET l_shipmode = '" + new_shipmode +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_shipmode = '" + shipmode +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ14Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Ratio is insensitive to +0.01 at SF1; use a large price bump so promo_revenue
  // moves beyond fingerprint precision.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  const int64_t price_raw = DecimalRaw(result.GetValue(2, row));
  constexpr int64_t kBumpRaw = 1000000000; // +10,000,000.00
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 10000000.0;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(5, row)),
                    Decimal(price_raw, 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Date(DateDays(result.GetValue(4, row)))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(price_raw + kBumpRaw, 2);
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + 10000000.00"
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ18Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Large quantity bump so SUM(l_quantity) / sum_qty in the answer moves.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(2, row));
  SharedProbe probe;
  probe.table = 0;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(quantity_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(0, row)), Decimal(quantity_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(quantity_raw + 100, 2); // +1.00
  probe.update_sql =
      "UPDATE lineitem SET l_quantity = l_quantity + 1.00 WHERE l_orderkey = " +
      orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql =
      "UPDATE lineitem SET l_quantity = " +
      std::to_string(static_cast<double>(quantity_raw) / 100.0) +
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ16Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto partkey = result.GetValue(0, row).ToString();
  const auto suppkey = result.GetValue(1, row).ToString();
  SharedProbe probe;
  probe.table = 3;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Int(result.GetValue(1, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::Int64,
                         static_cast<int64_t>(
                             std::get<int64_t>(probe.old_row.values[1].payload) +
                             1));
  probe.update_sql = "UPDATE partsupp SET ps_suppkey = ps_suppkey + 1 WHERE ps_partkey = " +
                     partkey + " AND ps_suppkey = " + suppkey;
  probe.restore_sql = "UPDATE partsupp SET ps_suppkey = " + suppkey +
                      " WHERE ps_partkey = " + partkey +
                      " AND ps_suppkey = " + std::to_string(std::stoll(suppkey) + 1);
  return probe;
}

SharedProbe BuildTpchQ17Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Bump extendedprice on a row already in the Q17 brand/container filter so
  // avg_yearly moves (tiny quantity bumps can be invisible after /7.0).
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int64_t quantity_raw = DecimalRaw(result.GetValue(3, row));
  const int64_t price_raw = DecimalRaw(result.GetValue(4, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(price_raw) / 100.0;
  probe.old_row = {{Int(result.GetValue(2, row)), Decimal(quantity_raw, 2),
                    Decimal(price_raw, 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(price_raw + 100, 2); // +1.00
  probe.update_sql =
      "UPDATE lineitem SET l_extendedprice = l_extendedprice + 1.00 WHERE l_orderkey = " +
      orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql =
      "UPDATE lineitem SET l_extendedprice = " +
      std::to_string(static_cast<double>(price_raw) / 100.0) +
      " WHERE l_orderkey = " + orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ2Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto partkey = result.GetValue(0, row).ToString();
  const auto suppkey = result.GetValue(1, row).ToString();
  const double cost = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 3; // partsupp
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1000.0;
  probe.current_value = cost;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 100000), 2);
  probe.update_sql = "UPDATE partsupp SET ps_supplycost = ps_supplycost + 1000.0"
                     " WHERE ps_partkey = " + partkey + " AND ps_suppkey = " +
                     suppkey;
  probe.restore_sql = "UPDATE partsupp SET ps_supplycost = " + std::to_string(cost) +
                      " WHERE ps_partkey = " + partkey + " AND ps_suppkey = " +
                      suppkey;
  return probe;
}

SharedProbe BuildTpchQ21Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Clear "late receipt" on a Q21-qualifying line so numwait drops.
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
  probe.new_row.values[3] = Date(commit_days);
  const auto commit_date =
      ::duckdb::Date::ToString(::duckdb::date_t{commit_days});
  const auto receipt_date =
      ::duckdb::Date::ToString(::duckdb::date_t{receipt_days});
  probe.update_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + commit_date +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + receipt_date +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
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

SharedProbe BuildTpchQ4Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // Clear late-receipt on a sole Q4-qualifying lineitem so EXISTS flips false
  // for that order (price bumps never change Q4). Row layout matches
  // TpchQ4Module::ReadLineitem: orderkey, commitdate, receiptdate.
  // table id must match TpchQ4Config{1, 2}.lineitem_table.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const int32_t commit_days = DateDays(result.GetValue(2, row));
  const int32_t receipt_days = DateDays(result.GetValue(3, row));
  SharedProbe probe;
  probe.table = 2;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 1.0;
  probe.current_value = static_cast<double>(receipt_days);
  probe.old_row = {{Int(result.GetValue(0, row)), Date(commit_days),
                    Date(receipt_days)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Date(commit_days);
  const auto commit_date =
      ::duckdb::Date::ToString(::duckdb::date_t{commit_days});
  const auto receipt_date =
      ::duckdb::Date::ToString(::duckdb::date_t{receipt_days});
  probe.update_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + commit_date +
                     "' WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_receiptdate = DATE '" + receipt_date +
                      "' WHERE l_orderkey = " + orderkey +
                      " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ19Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  // Select columns: orderkey, partkey, linenumber, qty, price, discount,
  // shipmode, shipinstruct. table=5 matches TpchQ19Config{5,6}.lineitem_table.
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto partkey = result.GetValue(1, row).ToString();
  const auto linenumber = result.GetValue(2, row).ToString();
  double quantity = 0.0;
  try {
    quantity = std::stod(result.GetValue(3, row).ToString());
  } catch (const std::exception &) {
    quantity = static_cast<double>(DecimalRaw(result.GetValue(3, row))) / 100.0;
  }
  SharedProbe probe;
  probe.table = 5;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 100.0;
  probe.current_value = quantity;
  probe.old_row = {{Int(result.GetValue(1, row)),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(5, row)), 2),
                    Text(result.GetValue(6, row)), Text(result.GetValue(7, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(3, row)) + 10000), 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100"
                     " WHERE l_orderkey = " + orderkey +
                     " AND l_partkey = " + partkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(quantity) + " WHERE l_orderkey = " +
                      orderkey + " AND l_partkey = " + partkey +
                      " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ7Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 4;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2)}};
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

SharedProbe BuildTpchQ8Probe(::duckdb::MaterializedQueryResult &result,
                             ::duckdb::idx_t row) {
  // AuxRow matches TpchQ8 ReadLineitem: partkey, orderkey, suppkey, price, discount.
  // table id matches TpchQ8Config{0,1,2,3,4}.lineitem_table = 4.
  // Large price bump so mkt_share fingerprint changes (tiny +0.01 can round away).
  constexpr int64_t kPriceBumpRaw = 10000; // +100.00 at scale 2
  const auto orderkey = result.GetValue(1, row).ToString();
  const auto linenumber = result.GetValue(5, row).ToString();
  const double price = std::stod(result.GetValue(3, row).ToString());
  const int64_t price_raw = DecimalRaw(result.GetValue(3, row));
  SharedProbe probe;
  probe.table = 4;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 100.0;
  probe.current_value = price;
  probe.old_row = {{Int(result.GetValue(0, row)), Int(result.GetValue(1, row)),
                    Int(result.GetValue(2, row)), Decimal(price_raw, 2),
                    Decimal(DecimalRaw(result.GetValue(4, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(price_raw + kPriceBumpRaw, 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 100.00"
                     " WHERE l_orderkey = " + orderkey +
                     " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ13Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto comment = result.GetValue(2, row).ToString();
  SharedProbe probe;
  probe.table = 1;  // TpchQ13Config{0,1}.orders_table
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {1};
  probe.write_delta = 1.0;
  probe.current_value = 0.0;
  probe.old_row = {{Int(result.GetValue(1, row)), Text(result.GetValue(2, row))}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] =
      core::AuxValue::Of(core::AuxType::String, comment + " unusual accounts");
  probe.update_sql =
      "UPDATE orders SET o_comment = o_comment || ' unusual accounts' "
      "WHERE o_orderkey = " + orderkey;
  probe.restore_sql = "UPDATE orders SET o_comment = " + SqlLiteral(comment) +
                      " WHERE o_orderkey = " + orderkey;
  return probe;
}

SharedProbe BuildTpchQ15Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double price = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 0.01;
  probe.current_value = price;
  probe.old_row = {{Date(DateDays(result.GetValue(4, row))),
                    Int(result.GetValue(5, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2),
                    Decimal(DecimalRaw(result.GetValue(3, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[2] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 1), 2);
  probe.update_sql = "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_extendedprice = " +
                      std::to_string(price) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ20Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto orderkey = result.GetValue(0, row).ToString();
  const auto linenumber = result.GetValue(1, row).ToString();
  const double quantity = std::stod(result.GetValue(2, row).ToString());
  SharedProbe probe;
  probe.table = 1;  // TpchQ20Config lineitem_table
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {3};
  probe.write_delta = 100000.0;
  probe.current_value = quantity;
  probe.old_row = {{Date(DateDays(result.GetValue(3, row))),
                    Int(result.GetValue(4, row)), Int(result.GetValue(5, row)),
                    Decimal(DecimalRaw(result.GetValue(2, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[3] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(2, row)) + 10000000), 2);
  probe.update_sql = "UPDATE lineitem SET l_quantity = l_quantity + 100000"
                     " WHERE l_orderkey = " +
                     orderkey + " AND l_linenumber = " + linenumber;
  probe.restore_sql = "UPDATE lineitem SET l_quantity = " +
                      std::to_string(quantity) + " WHERE l_orderkey = " +
                      orderkey + " AND l_linenumber = " + linenumber;
  return probe;
}

SharedProbe BuildTpchQ22Probe(::duckdb::MaterializedQueryResult &result,
                              ::duckdb::idx_t row) {
  const auto custkey = result.GetValue(0, row).ToString();
  const double acctbal = std::stod(result.GetValue(1, row).ToString());
  SharedProbe probe;
  probe.table = 9;
  probe.kind = core::MutationKind::Update;
  probe.updated_columns = {2};
  probe.write_delta = 0.01;
  probe.current_value = acctbal;
  probe.old_row = {{Int(result.GetValue(0, row)),
                    Decimal(DecimalRaw(result.GetValue(1, row)), 2)}};
  probe.new_row = probe.old_row;
  probe.new_row.values[1] = Decimal(
      static_cast<int64_t>(DecimalRaw(result.GetValue(1, row)) + 1), 2);
  probe.update_sql =
      "UPDATE customer SET c_acctbal = c_acctbal + 0.01 WHERE c_custkey = " +
      custkey;
  probe.restore_sql = "UPDATE customer SET c_acctbal = " + std::to_string(acctbal) +
                      " WHERE c_custkey = " + custkey;
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
     // Probe rows that participate in the TOP-10 so a price bump changes revenue.
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate "
     "FROM lineitem WHERE l_orderkey IN ("
     "SELECT l_orderkey FROM ("
     "SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, "
     "o_orderdate, o_shippriority "
     "FROM customer, orders, lineitem "
     "WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND "
     "l_orderkey = o_orderkey AND o_orderdate < DATE '1995-03-15' AND "
     "l_shipdate > DATE '1995-03-15' "
     "GROUP BY l_orderkey, o_orderdate, o_shippriority "
     "ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10))"
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
    {9,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ9Layout::kAccelerator,
     "SELECT nation, o_year, SUM(amount) AS sum_profit FROM ("
     "SELECT n_name AS nation, EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount "
     "FROM part, supplier, lineitem, partsupp, orders, nation "
     "WHERE s_suppkey = l_suppkey "
     "AND ps_suppkey = l_suppkey "
     "AND ps_partkey = l_partkey "
     "AND p_partkey = l_partkey "
     "AND o_orderkey = l_orderkey "
     "AND s_nationkey = n_nationkey "
     "AND p_name LIKE '%green%'"
     ") AS profit "
     "GROUP BY nation, o_year "
     "ORDER BY nation, o_year DESC",
     "SELECT * FROM accel_tpch_q9_aux('green')",
     "SELECT * FROM accel_tpch_q9('green')",
     "SELECT * FROM accel_tpch_q9_aux('green')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_suppkey, l_quantity, "
     "l_extendedprice, l_discount FROM lineitem "
     "WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '%green%') "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"green"})}},
     6,
     false,
     BuildTpchQ9Probe},
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
     "c_comment ORDER BY revenue DESC, c_custkey LIMIT 20",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
     "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
     // Returned lineitems that feed the TOP-20 customers so revenue moves.
     "SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount, "
     "l.l_returnflag FROM lineitem l "
     "JOIN orders o ON l.l_orderkey = o.o_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "WHERE l.l_returnflag = 'R' "
     "AND o.o_orderdate >= DATE '1993-06-01' "
     "AND o.o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
     "AND c.c_custkey IN ("
     "SELECT c_custkey FROM ("
     "SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM customer JOIN orders ON c_custkey = o_custkey "
     "JOIN lineitem ON l_orderkey = o_orderkey "
     "WHERE o_orderdate >= DATE '1993-06-01' "
     "AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH "
     "AND l_returnflag = 'R' "
     "GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20))"
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1993-06-01"))})}},
     4,
     false,
     BuildTpchQ10Probe},
    {11,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ11Layout::kAccelerator,
     "SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value "
     "FROM partsupp, supplier, nation "
     "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
     "AND n_name = 'ROMANIA' "
     "GROUP BY ps_partkey "
     "HAVING SUM(ps_supplycost * ps_availqty) > ("
     "SELECT SUM(ps_supplycost * ps_availqty) * 0.0001 "
     "FROM partsupp, supplier, nation "
     "WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey "
     "AND n_name = 'ROMANIA') "
     "ORDER BY value DESC",
     "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
     "SELECT * FROM accel_tpch_q11('ROMANIA', 0.0001)",
     "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
     "SELECT ps_partkey, ps_suppkey, ps_availqty, ps_supplycost "
     "FROM partsupp ps, supplier s, nation n "
     "WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey "
     "AND n.n_name = 'ROMANIA' "
     "ORDER BY ps_partkey, ps_suppkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"ROMANIA"}),
       core::AuxValue::Of(core::AuxType::Double, 0.0001)}},
     1,
     false,
     BuildTpchQ11Probe},
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
    {5,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ5Layout::kAccelerator,
     "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM customer, orders, lineitem, supplier, nation, region "
     "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey "
     "AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey "
     "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'AMERICA' "
     "AND o_orderdate >= DATE '1995-01-01' "
     "AND o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
     "GROUP BY n_name ORDER BY revenue DESC",
     "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q5('AMERICA', DATE '1995-01-01')",
     "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
     "SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount "
     "FROM lineitem l "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "JOIN supplier s ON s.s_suppkey = l.l_suppkey AND s.s_nationkey = c.c_nationkey "
     "JOIN nation n ON n.n_nationkey = s.s_nationkey "
     "JOIN region r ON r.r_regionkey = n.n_regionkey "
     "WHERE r.r_name = 'AMERICA' "
     "AND o.o_orderdate >= DATE '1995-01-01' "
     "AND o.o_orderdate < DATE '1995-01-01' + INTERVAL '1' year "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"AMERICA"}),
       core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1995-01-01"))})}},
     6,
     false,
     BuildTpchQ5Probe},
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
     "WHERE l_orderkey IN ("
     "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey "
     "HAVING SUM(l_quantity) > 312) "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, int64_t{312})}},
     0,
     false,
     BuildTpchQ18Probe},
    {16,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ16Layout::kAccelerator,
     "SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt "
     "FROM partsupp, part "
     "WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13' "
     "AND p_type NOT LIKE 'SMALL BRUSHED%' "
     "AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33) "
     "AND ps_suppkey NOT IN ("
     "SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%') "
     "GROUP BY p_brand, p_type, p_size "
     "ORDER BY supplier_cnt DESC, p_brand, p_type, p_size",
     "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     "SELECT * FROM accel_tpch_q16('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
     "SELECT ps_partkey, ps_suppkey FROM partsupp "
     "WHERE ps_partkey IN (SELECT p_partkey FROM part "
     "WHERE p_size IN (26, 24, 27, 29, 7, 20, 22, 33)) "
     "ORDER BY ps_partkey, ps_suppkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#13"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"SMALL BRUSHED%"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{26}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{24}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{27}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{29}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{7}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{20}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{22}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{33})}},
     3,
     false,
     BuildTpchQ16Probe},
    {17,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ17Layout::kAccelerator,
     "SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#54' AND p_container = 'MED PACK' "
     "AND l_quantity < avg_quantity",
     "SELECT * FROM accel_tpch_q17_aux('Brand#54', 'MED PACK')",
     "SELECT * FROM accel_tpch_q17('Brand#54', 'MED PACK')",
     "SELECT * FROM accel_tpch_q17_aux('Brand#54', 'MED PACK')",
     "SELECT l_orderkey, l_linenumber, l_partkey, l_quantity, l_extendedprice "
     "FROM lineitem, part, "
     "(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity "
     "FROM lineitem GROUP BY l_partkey) part_agg "
     "WHERE p_partkey = l_partkey AND agg_partkey = l_partkey "
     "AND p_brand = 'Brand#54' AND p_container = 'MED PACK' "
     "AND l_quantity < avg_quantity "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#54"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"MED PACK"})}},
     2,
     false,
     BuildTpchQ17Probe},
    {2,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ2Layout::kAccelerator,
     "SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment "
     "FROM part, supplier, partsupp, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15 "
     "AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'EUROPE' "
     "AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region "
     "WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey "
     "AND n_regionkey = r_regionkey AND r_name = 'EUROPE') "
     "ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100",
     "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
     "SELECT * FROM accel_tpch_q2(15, '%BRASS', 'EUROPE')",
     "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
     "SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost "
     "FROM part p, supplier s, partsupp ps, nation n, region r "
     "WHERE p.p_partkey = ps.ps_partkey AND s.s_suppkey = ps.ps_suppkey "
     "AND p.p_size = 15 AND p.p_type LIKE '%BRASS' "
     "AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey "
     "AND r.r_name = 'EUROPE' "
     "AND ps.ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, "
     "nation, region WHERE p.p_partkey = ps_partkey AND s_suppkey = ps_suppkey "
     "AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
     "AND r_name = 'EUROPE') "
     "ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Int64, static_cast<int64_t>(15)),
       core::AuxValue::Of(core::AuxType::String, std::string{"%BRASS"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"EUROPE"})}},
     3,
     false,
     BuildTpchQ2Probe},
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
     "SELECT l1.l_orderkey, l1.l_linenumber, l1.l_suppkey, l1.l_commitdate, "
     "l1.l_receiptdate "
     "FROM supplier, lineitem l1, orders, nation "
     "WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND "
     "o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND EXISTS ("
     "SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND "
     "l2.l_suppkey <> l1.l_suppkey) AND NOT EXISTS (SELECT * FROM lineitem l3 "
     "WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND "
     "l3.l_receiptdate > l3.l_commitdate) AND s_nationkey = n_nationkey AND "
     "n_name = 'SAUDI ARABIA' "
     "ORDER BY l1.l_orderkey, l1.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"SAUDI ARABIA"})}},
     3,
     false,
     BuildTpchQ21Probe},
    {4,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ4Layout::kAccelerator,
     "SELECT o_orderpriority, COUNT(o_orderkey) AS order_count "
     "FROM orders "
     "WHERE o_orderdate >= DATE '1993-07-01' "
     "AND o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
     "AND EXISTS ("
     "SELECT * FROM lineitem WHERE l_orderkey = o_orderkey "
     "AND l_commitdate < l_receiptdate) "
     "GROUP BY o_orderpriority ORDER BY o_orderpriority",
     "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
     "SELECT * FROM accel_tpch_q4(DATE '1993-07-01')",
     "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
     // Sole late-receipt lineitem on an in-window order so clearing receipt
     // drops the order from EXISTS / order_count.
     "SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate, l.l_receiptdate "
     "FROM lineitem l "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "WHERE o.o_orderdate >= DATE '1993-07-01' "
     "AND o.o_orderdate < DATE '1993-07-01' + INTERVAL '3' month "
     "AND l.l_commitdate < l.l_receiptdate "
     "AND (SELECT COUNT(*) FROM lineitem l2 "
     "WHERE l2.l_orderkey = l.l_orderkey "
     "AND l2.l_commitdate < l2.l_receiptdate) = 1 "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1993-07-01"))})}},
     2,
     false,
     BuildTpchQ4Probe},
    {7,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ7Layout::kAccelerator,
     "SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM ("
     "SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation, "
     "EXTRACT(year FROM l_shipdate) AS l_year, "
     "l_extendedprice * (1 - l_discount) AS volume "
     "FROM supplier, lineitem, orders, customer, nation n1, nation n2 "
     "WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey "
     "AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey "
     "AND c_nationkey = n2.n_nationkey "
     "AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL') "
     "OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM')) "
     "AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping "
     "GROUP BY supp_nation, cust_nation, l_year "
     "ORDER BY supp_nation, cust_nation, l_year",
     "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
     "SELECT * FROM accel_tpch_q7('UNITED KINGDOM', 'BRAZIL')",
     "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount FROM lineitem "
     "WHERE l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"UNITED KINGDOM"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"BRAZIL"})}},
     4,
     false,
     BuildTpchQ7Probe},
    {8,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ8Layout::kAccelerator,
     "SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) / "
     "SUM(volume) AS mkt_share FROM ("
     "SELECT EXTRACT(year FROM o_orderdate) AS o_year, "
     "l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation "
     "FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region "
     "WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey "
     "AND l_orderkey = o_orderkey AND o_custkey = c_custkey "
     "AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey "
     "AND r_name = 'ASIA' AND s_nationkey = n2.n_nationkey "
     "AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "AND p_type = 'PROMO BRUSHED BRASS') AS all_nations "
     "GROUP BY o_year ORDER BY o_year",
     "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     "SELECT * FROM accel_tpch_q8('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
     // Must join the full Q8 path (ASIA customer region) so +price changes mkt_share.
     "SELECT l.l_partkey, l.l_orderkey, l.l_suppkey, l.l_extendedprice, "
     "l.l_discount, l.l_linenumber FROM lineitem l "
     "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_type = 'PROMO BRUSHED BRASS' "
     "JOIN orders o ON o.o_orderkey = l.l_orderkey "
     "JOIN customer c ON c.c_custkey = o.o_custkey "
     "JOIN nation n1 ON n1.n_nationkey = c.c_nationkey "
     "JOIN region r ON r.r_regionkey = n1.n_regionkey AND r.r_name = 'ASIA' "
     "WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"INDIA"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"ASIA"}),
       core::AuxValue::Of(core::AuxType::String,
                          std::string{"PROMO BRUSHED BRASS"})}},
     4,
     false,
     BuildTpchQ8Probe},
    {13,
     SharedWorkloadKind::Tpch,
     "tpch",
     core::TpchQ13Layout::kAccelerator,
     "SELECT c_count, COUNT(*) AS custdist FROM ("
     "SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer "
     "LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE "
     "'%unusual%accounts%' GROUP BY c_custkey) AS c_orders "
     "GROUP BY c_count ORDER BY custdist DESC, c_count DESC",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
     "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
     "SELECT o_orderkey, o_custkey, o_comment FROM orders "
     "WHERE o_comment NOT LIKE '%unusual%accounts%' "
     "ORDER BY o_orderkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String,
                          std::string{"%unusual%accounts%"})}},
     1,
     false,
     BuildTpchQ13Probe},
    {15,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ15Layout::kAccelerator,
     "SELECT s_suppkey, s_name, s_address, s_phone, total_revenue "
     "FROM supplier, ("
     "SELECT l_suppkey AS supplier_no, "
     "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
     "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey) revenue0 "
     "WHERE s_suppkey = supplier_no "
     "AND total_revenue = (SELECT MAX(total_revenue) FROM ("
     "SELECT l_suppkey AS supplier_no, "
     "SUM(l_extendedprice * (1 - l_discount)) AS total_revenue "
     "FROM lineitem WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey) revenue1) ORDER BY s_suppkey",
     "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
     "SELECT * FROM accel_tpch_q15(DATE '1994-11-01')",
     "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
     "SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, "
     "l_suppkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "AND l_suppkey = (SELECT l_suppkey FROM lineitem "
     "WHERE l_shipdate >= DATE '1994-11-01' "
     "AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH "
     "GROUP BY l_suppkey "
     "ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1) "
     "ORDER BY l_orderkey, l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1994-11-01"))})}},
     1,
     false,
     BuildTpchQ15Probe},
    {19,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ19Layout::kAccelerator,
     "SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue "
     "FROM lineitem, part "
     "WHERE (p_partkey = l_partkey "
     "AND p_brand = 'Brand#31' "
     "AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l_quantity >= 4 AND l_quantity <= 4 + 10 "
     "AND p_size BETWEEN 1 AND 5 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#12' "
     "AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') "
     "AND l_quantity >= 15 AND l_quantity <= 15 + 10 "
     "AND p_size BETWEEN 1 AND 10 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON') "
     "OR (p_partkey = l_partkey "
     "AND p_brand = 'Brand#51' "
     "AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') "
     "AND l_quantity >= 23 AND l_quantity <= 23 + 10 "
     "AND p_size BETWEEN 1 AND 15 "
     "AND l_shipmode IN ('AIR', 'AIR REG') "
     "AND l_shipinstruct = 'DELIVER IN PERSON')",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
     "SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity, "
     "l.l_extendedprice, l.l_discount, l.l_shipmode, l.l_shipinstruct "
     "FROM lineitem l, part p "
     "WHERE p.p_partkey = l.l_partkey AND p.p_brand = 'Brand#31' "
     "AND p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') "
     "AND l.l_quantity >= 4 AND l.l_quantity <= 14 "
     "AND p.p_size BETWEEN 1 AND 5 "
     "AND l.l_shipmode IN ('AIR', 'AIR REG') "
     "AND l.l_shipinstruct = 'DELIVER IN PERSON' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"Brand#31"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{4}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#12"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{15}),
       core::AuxValue::Of(core::AuxType::String, std::string{"Brand#51"}),
       core::AuxValue::Of(core::AuxType::Int64, int64_t{23})}},
     5,
     false,
     BuildTpchQ19Probe},
    {20,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ20Layout::kAccelerator,
     "SELECT s_name, s_address "
     "FROM supplier, nation "
     "WHERE s_suppkey IN ("
     "SELECT ps_suppkey FROM partsupp, ("
     "SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity "
     "FROM lineitem WHERE l_shipdate >= DATE '1997-01-01' "
     "AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year "
     "GROUP BY l_partkey, l_suppkey) agg_lineitem "
     "WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey "
     "AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%') "
     "AND ps_availqty > agg_quantity) "
     "AND s_nationkey = n_nationkey AND n_name = 'RUSSIA' "
     "ORDER BY s_name",
     "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT * FROM accel_tpch_q20(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
     "SELECT l.l_orderkey, l.l_linenumber, l.l_quantity, l.l_shipdate, "
     "l.l_partkey, l.l_suppkey FROM lineitem l "
     "JOIN part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE 'gainsboro%' "
     "JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey "
     "JOIN supplier s ON s.s_suppkey = l.l_suppkey "
     "JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA' "
     "WHERE l.l_shipdate >= DATE '1997-01-01' "
     "AND l.l_shipdate < DATE '1998-01-01' "
     "ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::Date,
                          core::AuxDate{::duckdb::Date::EpochDays(
                              ::duckdb::Date::FromString("1997-01-01"))}),
       core::AuxValue::Of(core::AuxType::String, std::string{"gainsboro%"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"RUSSIA"})}},
     1,
     false,
     BuildTpchQ20Probe},
    {22,
     SharedWorkloadKind::Tpch,
     "tpch",
     query::TpchQ22Layout::kAccelerator,
     "WITH custsale AS ("
     "SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal "
     "FROM customer "
     "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32') "
     "AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00 "
     "AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')) "
     "AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)) "
     "SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal "
     "FROM custsale GROUP BY cntrycode ORDER BY cntrycode",
     "SELECT * FROM accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
     "SELECT * FROM accel_tpch_q22('30', '19', '14', '28', '11', '22', '32')",
     "SELECT * FROM accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
     "SELECT c_custkey, c_acctbal FROM customer "
     "WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN "
     "('30', '19', '14', '28', '11', '22', '32') "
     "ORDER BY c_custkey LIMIT 1",
     {{core::AuxValue::Of(core::AuxType::String, std::string{"30"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"19"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"14"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"28"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"11"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"22"}),
       core::AuxValue::Of(core::AuxType::String, std::string{"32"})}},
     9,
     false,
     BuildTpchQ22Probe},
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
  std::vector<int> out;
  if (workload == SharedWorkloadKind::Tpch) {
    for (const auto &entry : kTpchCatalog) {
      out.push_back(entry.query_id);
    }
    return out;
  }
  for (const auto &entry : kSecEdgarCatalog) {
    out.push_back(entry.query_id);
  }
  return out;
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

std::string ExpandProbeSelectLimit(const char *probe_select_sql, int limit) {
  Require(probe_select_sql != nullptr && *probe_select_sql,
          "missing probe_select_sql");
  std::string sql(probe_select_sql);
  const auto pos = sql.rfind("LIMIT");
  if (pos != std::string::npos) {
    return sql.substr(0, pos) + "LIMIT " + std::to_string(limit);
  }
  return sql + " LIMIT " + std::to_string(limit);
}

std::string NormalizeFingerprintValue(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return "NULL";
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DATE) {
    const auto date = value.GetValue<::duckdb::date_t>();
    return ::duckdb::Date::ToString(date);
  }
  try {
    const auto source = value.ToString();
    std::size_t parsed = 0;
    const auto numeric = std::stold(source, &parsed);
    if (parsed != source.size()) {
      return source;
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << numeric;
    return text.str();
  } catch (...) {
    return value.ToString();
  }
}

std::string FingerprintSharedBaseSql(::duckdb::Connection &connection,
                                     const SharedQueryCatalogEntry &entry) {
  Require(entry.base_sql != nullptr && *entry.base_sql,
          "missing base_sql for shared fingerprint");
  auto base_result = connection.Query(entry.base_sql);
  Require(base_result != nullptr && !base_result->HasError(),
          base_result ? base_result->GetError()
                      : "DuckDB fingerprint query failed");
  auto &base = base_result->Cast<::duckdb::MaterializedQueryResult>();
  std::ostringstream out;
  out << "rows=" << base.RowCount() << '\n';
  for (::duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
    for (::duckdb::idx_t col = 0; col < base.ColumnCount(); ++col) {
      if (col > 0) {
        out << '\t';
      }
      out << NormalizeFingerprintValue(base.GetValue(col, row));
    }
    out << '\n';
  }
  return out.str();
}

bool SharedProbeChangesBaseResult(::duckdb::Connection &connection,
                                  const SharedQueryCatalogEntry &entry,
                                  const SharedProbe &probe,
                                  const std::string &before_fp) {
  connection.BeginTransaction();
  auto updated = connection.Query(probe.update_sql);
  if (updated == nullptr || updated->HasError()) {
    connection.Rollback();
    return false;
  }
  const auto after_fp = FingerprintSharedBaseSql(connection, entry);
  connection.Rollback();
  return after_fp != before_fp;
}

std::vector<SharedProbe> CollectInvalidatingSharedProbes(
    ::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
    int candidate_limit, int max_keep) {
  Require(static_cast<bool>(entry.build_probe),
          "missing build_probe for Q" + std::to_string(entry.query_id));
  Require(candidate_limit > 0 && max_keep > 0,
          "invalidating probe limits must be positive");
  const auto before_fp = FingerprintSharedBaseSql(connection, entry);
  const std::string probe_sql =
      ExpandProbeSelectLimit(entry.probe_select_sql, candidate_limit);
  auto result = connection.Query(probe_sql);
  Require(result != nullptr && !result->HasError() && result->RowCount() >= 1,
          result ? ("invalidating probe select failed for Q" +
                    std::to_string(entry.query_id) + ": " + result->GetError())
                 : "invalidating probe select returned null");
  auto &materialized = result->Cast<::duckdb::MaterializedQueryResult>();
  std::vector<SharedProbe> probes;
  probes.reserve(static_cast<std::size_t>(
      std::min<int>(max_keep, static_cast<int>(materialized.RowCount()))));
  for (::duckdb::idx_t row = 0; row < materialized.RowCount(); ++row) {
    auto probe = entry.build_probe(materialized, row);
    if (!SharedProbeChangesBaseResult(connection, entry, probe, before_fp)) {
      continue;
    }
    probes.push_back(std::move(probe));
    if (static_cast<int>(probes.size()) >= max_keep) {
      break;
    }
  }
  Require(!probes.empty(),
          "no shared probe candidate changed DuckDB Q" +
              std::to_string(entry.query_id) +
              " result (catalog probe_select/build_probe must invalidate this "
              "query so write-frequency benches exercise maintenance; if "
              "base_sql is empty, fix catalog seed bindings first)");
  return probes;
}

SharedProbe FindInvalidatingSharedProbe(::duckdb::Connection &connection,
                                        const SharedQueryCatalogEntry &entry) {
  auto probes = CollectInvalidatingSharedProbes(connection, entry,
                                                /*candidate_limit=*/256,
                                                /*max_keep=*/1);
  return probes.front();
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
