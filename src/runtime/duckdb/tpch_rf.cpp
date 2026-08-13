#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb/common/types/date.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;

void Require(bool ok, const std::string &msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
}

core::AuxValue IntVal(int64_t v) {
  return core::AuxValue::Of(core::AuxType::Int64, v);
}
core::AuxValue DecVal(int64_t raw, int32_t scale) {
  return core::AuxValue::Of(core::AuxType::ScaledDecimal,
                            core::ScaledDecimal{raw, scale});
}
core::AuxValue DateVal(int32_t days) {
  return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days});
}
core::AuxValue StrVal(std::string s) {
  return core::AuxValue::Of(core::AuxType::String, std::move(s));
}

int64_t DecimalRaw2(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DECIMAL) {
    const auto scale =
        static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()));
    int64_t raw = 0;
    switch (value.type().InternalType()) {
    case ::duckdb::PhysicalType::INT16:
      raw = value.GetValueUnsafe<int16_t>();
      break;
    case ::duckdb::PhysicalType::INT32:
      raw = value.GetValueUnsafe<int32_t>();
      break;
    case ::duckdb::PhysicalType::INT64:
      raw = value.GetValueUnsafe<int64_t>();
      break;
    case ::duckdb::PhysicalType::INT128: {
      const auto huge = value.GetValueUnsafe<::duckdb::hugeint_t>();
      raw = static_cast<int64_t>(huge.lower);
      break;
    }
    default:
      return static_cast<int64_t>(
          std::llround(std::stod(value.ToString()) * 100.0));
    }
    int32_t delta = 2 - scale;
    if (delta > 0) {
      for (int32_t i = 0; i < delta; ++i) {
        raw *= 10;
      }
    } else if (delta < 0) {
      for (int32_t i = 0; i < -delta; ++i) {
        raw /= 10;
      }
    }
    return raw;
  }
  return static_cast<int64_t>(std::llround(std::stod(value.ToString()) * 100.0));
}

int64_t SafeI64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::BIGINT) {
    return value.GetValue<int64_t>();
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::INTEGER) {
    return value.GetValue<int32_t>();
  }
  return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
}

int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DATE) {
    return ::duckdb::Date::EpochDays(value.GetValue<::duckdb::date_t>());
  }
  return static_cast<int32_t>(SafeI64(value));
}

std::string SqlInList(const std::vector<int64_t> &keys) {
  Require(!keys.empty(), "RF chunk has empty orderkeys");
  std::ostringstream oss;
  oss << "(";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i) {
      oss << ",";
    }
    oss << keys[i];
  }
  oss << ")";
  return oss.str();
}

std::string SqlQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

std::string DateLiteral(const ::duckdb::Value &value) {
  return "DATE " + SqlQuote(value.ToString());
}

std::string DecLiteral(const ::duckdb::Value &value) {
  return value.ToString();
}

// Build AuxRows matching each module's AccumulateMutation Read* layouts.
core::AuxRow LineitemAuxRowForQuery(int query_id,
                                    ::duckdb::MaterializedQueryResult &li,
                                    ::duckdb::idx_t row) {
  // Common SELECT columns (alias order):
  // 0 l_orderkey, 1 l_partkey, 2 l_suppkey, 3 l_linenumber,
  // 4 l_quantity, 5 l_extendedprice, 6 l_discount, 7 l_tax,
  // 8 l_returnflag, 9 l_linestatus,
  // 10 l_shipdate, 11 l_commitdate, 12 l_receiptdate,
  // 13 l_shipinstruct, 14 l_shipmode, 15 l_comment
  const auto orderkey = SafeI64(li.GetValue(0, row));
  const auto partkey = SafeI64(li.GetValue(1, row));
  const auto suppkey = SafeI64(li.GetValue(2, row));
  const auto qty = DecimalRaw2(li.GetValue(4, row));
  const auto price = DecimalRaw2(li.GetValue(5, row));
  const auto discount = DecimalRaw2(li.GetValue(6, row));
  const auto tax = DecimalRaw2(li.GetValue(7, row));
  const auto returnflag = li.GetValue(8, row).ToString();
  const auto linestatus = li.GetValue(9, row).ToString();
  const auto shipdate = DateDays(li.GetValue(10, row));
  const auto commitdate = DateDays(li.GetValue(11, row));
  const auto receiptdate = DateDays(li.GetValue(12, row));
  const auto shipmode = li.GetValue(14, row).ToString();

  switch (query_id) {
  case 1:
    return {{DateVal(shipdate), DecVal(qty, 2), DecVal(price, 2),
             DecVal(discount, 2), DecVal(tax, 2), StrVal(returnflag),
             StrVal(linestatus)}};
  case 3:
    return {{IntVal(orderkey), DecVal(price, 2), DecVal(discount, 2),
             DateVal(shipdate)}};
  case 6:
    return {{DateVal(shipdate), DecVal(discount, 2), DecVal(qty, 2),
             DecVal(price, 2)}};
  case 9:
    return {{IntVal(partkey), IntVal(suppkey), IntVal(orderkey), DecVal(qty, 2),
             DecVal(price, 2), DecVal(discount, 2)}};
  case 10:
    return {{IntVal(orderkey), DecVal(price, 2), DecVal(discount, 2),
             StrVal(returnflag)}};
  case 12:
    return {{IntVal(orderkey), StrVal(shipmode), DateVal(commitdate),
             DateVal(receiptdate), DateVal(shipdate)}};
  case 14:
    return {{IntVal(partkey), DecVal(price, 2), DecVal(discount, 2),
             DateVal(shipdate)}};
  case 17:
    return {{IntVal(partkey), DecVal(qty, 2), DecVal(price, 2)}};
  case 18:
    return {{IntVal(orderkey), DecVal(qty, 2)}};
  case 21:
    return {{IntVal(orderkey), IntVal(suppkey), DateVal(commitdate),
             DateVal(receiptdate)}};
  default:
    return {{IntVal(orderkey), DecVal(qty, 2), DecVal(price, 2)}};
  }
}

core::AuxRow OrderAuxRowForQuery(int query_id,
                                 ::duckdb::MaterializedQueryResult &ord,
                                 ::duckdb::idx_t row) {
  // 0 o_orderkey, 1 o_custkey, 2 o_orderstatus, 3 o_totalprice,
  // 4 o_orderdate, 5 o_orderpriority, 6 o_clerk, 7 o_shippriority, 8 o_comment
  const auto orderkey = SafeI64(ord.GetValue(0, row));
  const auto custkey = SafeI64(ord.GetValue(1, row));
  const auto status = ord.GetValue(2, row).ToString();
  const auto totalprice = DecimalRaw2(ord.GetValue(3, row));
  const auto orderdate = DateDays(ord.GetValue(4, row));
  const auto priority = ord.GetValue(5, row).ToString();
  const auto shippriority = SafeI64(ord.GetValue(7, row));

  switch (query_id) {
  case 3:
    return {{IntVal(orderkey), DateVal(orderdate), IntVal(custkey),
             IntVal(shippriority)}};
  case 9:
    return {{IntVal(orderkey), DateVal(orderdate)}};
  case 10:
    return {{IntVal(orderkey), IntVal(custkey), DateVal(orderdate)}};
  case 12:
    return {{IntVal(orderkey), StrVal(priority)}};
  case 18:
    return {{IntVal(orderkey), IntVal(custkey), DateVal(orderdate),
             DecVal(totalprice, 2)}};
  case 21:
    return {{IntVal(orderkey),
             StrVal(status.empty() ? std::string("F") : status.substr(0, 1))}};
  default:
    return {{IntVal(orderkey), IntVal(custkey), DateVal(orderdate)}};
  }
}

std::vector<int64_t> PickDeleteOrderkeys(::duckdb::Connection &connection,
                                         int order_count, int write_seq) {
  // Stable-ish rotating window over existing keys.
  auto count_res = connection.Query("SELECT COUNT(*)::BIGINT FROM orders");
  Require(count_res && !count_res->HasError() && count_res->RowCount() > 0,
          "RF2 count orders failed");
  const auto n = SafeI64(count_res->GetValue(0, 0));
  Require(n > 0, "RF2: orders table empty");
  const auto take = std::min<int64_t>(order_count, n);
  const auto offset = (static_cast<int64_t>(write_seq / 2) * take) % n;
  auto res = connection.Query(
      "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " +
      std::to_string(take) + " OFFSET " + std::to_string(offset));
  Require(res && !res->HasError() && res->RowCount() > 0, "RF2 pick keys failed");
  auto &mat = res->Cast<::duckdb::MaterializedQueryResult>();
  std::vector<int64_t> keys;
  keys.reserve(mat.RowCount());
  for (::duckdb::idx_t i = 0; i < mat.RowCount(); ++i) {
    keys.push_back(SafeI64(mat.GetValue(0, i)));
  }
  return keys;
}

} // namespace

TpchRfTableIds TpchRfTableIdsForQuery(int query_id) {
  // Must match CreateBinding configs in duckdb_runtime.cpp.
  TpchRfTableIds ids;
  switch (query_id) {
  case 1:
  case 6:
    ids.lineitem = 1;
    ids.watches_lineitem = true;
    break;
  case 3:
    ids.orders = 2;
    ids.lineitem = 3;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 9:
    ids.orders = 4;
    ids.lineitem = 6;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 10:
    ids.orders = 3;
    ids.lineitem = 4;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 12:
    ids.orders = 1;
    ids.lineitem = 2;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 14:
  case 17:
    ids.lineitem = 2;
    ids.watches_lineitem = true;
    break;
  case 18:
    ids.lineitem = 0;
    ids.orders = 1;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 21:
    ids.orders = 2;
    ids.lineitem = 3;
    ids.watches_orders = true;
    ids.watches_lineitem = true;
    break;
  case 11:
  default:
    break;
  }
  return ids;
}

std::string TpchRfKindName(TpchRfKind kind) {
  return kind == TpchRfKind::Rf1Insert ? "rf1" : "rf2";
}

TpchRfChunkPlan NextTpchRfChunkPlan(int write_seq, int order_count,
                                    ::duckdb::Connection &connection) {
  TpchRfChunkPlan plan;
  plan.order_count = std::max(1, order_count);
  // Even seq => RF2 delete; odd => RF1 insert.
  if ((write_seq % 2) == 0) {
    plan.kind = TpchRfKind::Rf2Delete;
    plan.orderkeys = PickDeleteOrderkeys(connection, plan.order_count, write_seq);
  } else {
    plan.kind = TpchRfKind::Rf1Insert;
  }
  return plan;
}

TpchRfApplyResult ApplyTpchRfChunkSql(::duckdb::Connection &connection,
                                      int query_id, const TpchRfChunkPlan &plan) {
  TpchRfApplyResult out;
  const auto ids = TpchRfTableIdsForQuery(query_id);
  constexpr const char *kLiCols =
      "l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, "
      "l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, "
      "l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, "
      "l_comment";
  constexpr const char *kOrdCols =
      "o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, "
      "o_orderpriority, o_clerk, o_shippriority, o_comment";

  if (plan.kind == TpchRfKind::Rf2Delete) {
    Require(!plan.orderkeys.empty(), "RF2 requires orderkeys");
    const auto in_list = SqlInList(plan.orderkeys);

    auto li_res = connection.Query(std::string("SELECT ") + kLiCols +
                                   " FROM lineitem WHERE l_orderkey IN " +
                                   in_list);
    Require(li_res && !li_res->HasError(),
            li_res ? li_res->GetError() : "RF2 lineitem select failed");
    auto &li = li_res->Cast<::duckdb::MaterializedQueryResult>();

    auto ord_res = connection.Query(std::string("SELECT ") + kOrdCols +
                                    " FROM orders WHERE o_orderkey IN " +
                                    in_list);
    Require(ord_res && !ord_res->HasError(),
            ord_res ? ord_res->GetError() : "RF2 orders select failed");
    auto &ord = ord_res->Cast<::duckdb::MaterializedQueryResult>();

    auto del_li =
        connection.Query("DELETE FROM lineitem WHERE l_orderkey IN " + in_list);
    Require(del_li && !del_li->HasError(),
            del_li ? del_li->GetError() : "RF2 delete lineitem failed");
    auto del_ord =
        connection.Query("DELETE FROM orders WHERE o_orderkey IN " + in_list);
    Require(del_ord && !del_ord->HasError(),
            del_ord ? del_ord->GetError() : "RF2 delete orders failed");

    if (ids.watches_lineitem) {
      core::MutationBatch batch;
      batch.table = ids.lineitem;
      batch.kind = core::MutationKind::Delete;
      for (::duckdb::idx_t i = 0; i < li.RowCount(); ++i) {
        batch.old_rows.push_back(LineitemAuxRowForQuery(query_id, li, i));
      }
      out.mutations.push_back(std::move(batch));
    }
    if (ids.watches_orders) {
      core::MutationBatch batch;
      batch.table = ids.orders;
      batch.kind = core::MutationKind::Delete;
      for (::duckdb::idx_t i = 0; i < ord.RowCount(); ++i) {
        batch.old_rows.push_back(OrderAuxRowForQuery(query_id, ord, i));
      }
      out.mutations.push_back(std::move(batch));
    }
    out.summary = "rf2_delete orders=" + std::to_string(plan.orderkeys.size()) +
                  " lineitems=" + std::to_string(li.RowCount());
    return out;
  }

  // RF1: clone a small template order set under fresh keys.
  auto max_res = connection.Query("SELECT COALESCE(MAX(o_orderkey),0)::BIGINT FROM orders");
  Require(max_res && !max_res->HasError(), "RF1 max orderkey failed");
  int64_t next_key = SafeI64(max_res->GetValue(0, 0)) + 1;

  auto tmpl_ord = connection.Query(
      std::string("SELECT ") + kOrdCols +
      " FROM orders ORDER BY o_orderkey LIMIT " + std::to_string(plan.order_count));
  Require(tmpl_ord && !tmpl_ord->HasError() && tmpl_ord->RowCount() > 0,
          "RF1 template orders failed");
  auto &tord = tmpl_ord->Cast<::duckdb::MaterializedQueryResult>();

  std::vector<int64_t> old_keys;
  std::vector<int64_t> new_keys;
  old_keys.reserve(tord.RowCount());
  new_keys.reserve(tord.RowCount());
  for (::duckdb::idx_t i = 0; i < tord.RowCount(); ++i) {
    old_keys.push_back(SafeI64(tord.GetValue(0, i)));
    new_keys.push_back(next_key++);
  }

  // Insert cloned orders.
  for (::duckdb::idx_t i = 0; i < tord.RowCount(); ++i) {
    std::ostringstream sql;
    sql << "INSERT INTO orders VALUES (" << new_keys[i] << ", "
        << SafeI64(tord.GetValue(1, i)) << ", "
        << SqlQuote(tord.GetValue(2, i).ToString()) << ", "
        << DecLiteral(tord.GetValue(3, i)) << ", "
        << DateLiteral(tord.GetValue(4, i)) << ", "
        << SqlQuote(tord.GetValue(5, i).ToString()) << ", "
        << SqlQuote(tord.GetValue(6, i).ToString()) << ", "
        << SafeI64(tord.GetValue(7, i)) << ", "
        << SqlQuote(tord.GetValue(8, i).ToString()) << ")";
    auto ins = connection.Query(sql.str());
    Require(ins && !ins->HasError(),
            ins ? ins->GetError() : "RF1 insert order failed");
  }

  // Clone lineitems for those template keys onto new keys.
  std::size_t lineitem_count = 0;
  for (std::size_t oi = 0; oi < old_keys.size(); ++oi) {
    auto li_res = connection.Query(std::string("SELECT ") + kLiCols +
                                   " FROM lineitem WHERE l_orderkey = " +
                                   std::to_string(old_keys[oi]));
    Require(li_res && !li_res->HasError(),
            li_res ? li_res->GetError() : "RF1 template lineitem failed");
    auto &li = li_res->Cast<::duckdb::MaterializedQueryResult>();
    for (::duckdb::idx_t r = 0; r < li.RowCount(); ++r) {
      std::ostringstream sql;
      sql << "INSERT INTO lineitem VALUES (" << new_keys[oi] << ", "
          << SafeI64(li.GetValue(1, r)) << ", " << SafeI64(li.GetValue(2, r))
          << ", " << SafeI64(li.GetValue(3, r)) << ", "
          << DecLiteral(li.GetValue(4, r)) << ", "
          << DecLiteral(li.GetValue(5, r)) << ", "
          << DecLiteral(li.GetValue(6, r)) << ", "
          << DecLiteral(li.GetValue(7, r)) << ", "
          << SqlQuote(li.GetValue(8, r).ToString()) << ", "
          << SqlQuote(li.GetValue(9, r).ToString()) << ", "
          << DateLiteral(li.GetValue(10, r)) << ", "
          << DateLiteral(li.GetValue(11, r)) << ", "
          << DateLiteral(li.GetValue(12, r)) << ", "
          << SqlQuote(li.GetValue(13, r).ToString()) << ", "
          << SqlQuote(li.GetValue(14, r).ToString()) << ", "
          << SqlQuote(li.GetValue(15, r).ToString()) << ")";
      auto ins = connection.Query(sql.str());
      Require(ins && !ins->HasError(),
              ins ? ins->GetError() : "RF1 insert lineitem failed");
      ++lineitem_count;
    }
  }

  // Re-read inserted rows for mutation AuxRows (fresh keys).
  const auto new_in = SqlInList(new_keys);
  auto new_ord = connection.Query(std::string("SELECT ") + kOrdCols +
                                  " FROM orders WHERE o_orderkey IN " + new_in);
  Require(new_ord && !new_ord->HasError(), "RF1 reread orders failed");
  auto &nord = new_ord->Cast<::duckdb::MaterializedQueryResult>();
  auto new_li = connection.Query(std::string("SELECT ") + kLiCols +
                                 " FROM lineitem WHERE l_orderkey IN " + new_in);
  Require(new_li && !new_li->HasError(), "RF1 reread lineitem failed");
  auto &nli = new_li->Cast<::duckdb::MaterializedQueryResult>();

  if (ids.watches_orders) {
    core::MutationBatch batch;
    batch.table = ids.orders;
    batch.kind = core::MutationKind::Insert;
    for (::duckdb::idx_t i = 0; i < nord.RowCount(); ++i) {
      batch.new_rows.push_back(OrderAuxRowForQuery(query_id, nord, i));
    }
    out.mutations.push_back(std::move(batch));
  }
  if (ids.watches_lineitem) {
    core::MutationBatch batch;
    batch.table = ids.lineitem;
    batch.kind = core::MutationKind::Insert;
    for (::duckdb::idx_t i = 0; i < nli.RowCount(); ++i) {
      batch.new_rows.push_back(LineitemAuxRowForQuery(query_id, nli, i));
    }
    out.mutations.push_back(std::move(batch));
  }

  out.summary = "rf1_insert orders=" + std::to_string(new_keys.size()) +
                " lineitems=" + std::to_string(lineitem_count);
  return out;
}

} // namespace accelerator_codegen::aux_state::engine
