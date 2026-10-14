// SCAFFOLDING
#include "eval/harness/tpch_rf.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>

#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::harness {
namespace {

void Require(bool ok, const std::string &msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
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

std::string TpchRfChunkStatements(::duckdb::Connection &connection, const TpchRfChunkPlan &plan) {
  std::ostringstream sql;
  if (plan.kind == TpchRfKind::Rf2Delete) {
    Require(!plan.orderkeys.empty(), "RF2 requires orderkeys");
    const auto in_list = SqlInList(plan.orderkeys);
    sql << "DELETE FROM lineitem WHERE l_orderkey IN " << in_list << "; DELETE FROM orders WHERE o_orderkey IN "
        << in_list;
    return sql.str();
  }
  auto max_res = connection.Query("SELECT COALESCE(MAX(o_orderkey),0)::BIGINT FROM orders");
  Require(max_res && !max_res->HasError(), "RF1 max orderkey failed");
  int64_t next_key = SafeI64(max_res->GetValue(0, 0)) + 1;
  auto tmpl = connection.Query("SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT " +
                               std::to_string(plan.order_count));
  Require(tmpl && !tmpl->HasError() && tmpl->RowCount() > 0, "RF1 template orders failed");
  for (::duckdb::idx_t i = 0; i < tmpl->RowCount(); ++i) {
    const auto from = std::to_string(SafeI64(tmpl->GetValue(0, i)));
    const auto key = std::to_string(next_key++);
    sql << (i ? "; " : "") << "INSERT INTO orders SELECT * REPLACE (" << key
        << " AS o_orderkey) FROM orders WHERE o_orderkey = " << from << "; INSERT INTO lineitem SELECT * REPLACE ("
        << key << " AS l_orderkey) FROM lineitem WHERE l_orderkey = " << from;
  }
  return sql.str();
}

void ApplyTpchRfChunk(::duckdb::Connection &connection,
                      engine::duckdb::DuckdbRuntime &runtime, int write_seq,
                      int order_count) {
  const bool was_capturing = runtime.MutationCaptureEnabled();
  runtime.EnableMutationCapture();
  try {
    runtime.ApplyWrite(TpchRfChunkStatements(connection, NextTpchRfChunkPlan(write_seq, order_count, connection)));
  } catch (...) {
    if (!was_capturing) {
      runtime.DisableMutationCapture();
    }
    throw;
  }
  if (!was_capturing) {
    runtime.DisableMutationCapture();
  }
}

} // namespace accelerator_codegen::aux_state::harness
