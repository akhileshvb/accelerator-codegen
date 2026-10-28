// SCAFFOLDING
#include "eval/harness/differential.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "eval/harness/float_order_ties.hpp"
#include "eval/harness/suite.hpp"
#include "eval/harness/tpch_rf.hpp"
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::harness {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace aux_engine = ::accelerator_codegen::aux_state::engine;
using aux_engine::duckdb::DuckdbRuntime;
using aux_engine::SharedQueryCatalogEntry;
using aux_engine::SharedProbe;
using aux_engine::BindArguments;
using aux_engine::DefaultArguments;
using aux_engine::CollectInvalidatingSharedProbes;
using aux_engine::FindSharedProbe;
using aux_engine::FindInvalidatingSharedProbe;
using aux_engine::FingerprintSharedBaseSql;
using aux_engine::SharedProbeChangesBaseResult;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// ACCEL_LOG_MAINTENANCE_ACTIONS=1: what the policy chose for each commit.
void LogMaintenanceAction(DuckdbRuntime &runtime, const std::string &what) {
  if (!std::getenv("ACCEL_LOG_MAINTENANCE_ACTIONS")) {
    return;
  }
  const auto entry = runtime.Stats().last_decision;
  std::cerr << "maintenance_action="
            << (entry ? maintenance::MaintenanceActionName(entry->staged_action) : "none")
            << (what.empty() ? "" : " after " + what) << '\n';
}

void RequireForcedMaintenanceAction(DuckdbRuntime &runtime) {
  const char *force = std::getenv("ACCEL_FORCE_MAINTENANCE_ACTION");
  const std::string force_s = force ? force : "";
  if (force_s.empty()) {
    LogMaintenanceAction(runtime, "");
    return;
  }
  core::SetForcedMaintenanceActionName(force_s);
  // The commit's decision lives in the accelerator's stats: the commit logs on its own thread.
  auto entry = runtime.Stats().last_decision;
  if (!entry) {
    entry = core::LastMaintenanceDecision();
  }
  Require(entry && entry->valid,
          "expected staged maintenance decision after Apply (force=" + force_s +
              ")");
  const std::string action =
      maintenance::MaintenanceActionName(entry->staged_action);
  if (force_s != "NoOp" && entry->staged_action == maintenance::MaintenanceAction::NoOp) {
    // The module applies changes this small incrementally without asking the
    // policy (it consults it for bulk changes only): nothing to force here.
    std::cerr << "maintenance_action=not-consulted force=" << force_s << " (N/A)\n";
    return;
  }
  if (force_s == "ApplyDelta") {
    Require(action == "ApplyDelta",
            "expected ApplyDelta maintenance but got action=[" + action +
                "] force=" + force_s);
  } else if (force_s == "RebuildPartition") {
    Require(action == "RebuildPartition" || action == "RebuildAll",
            "forced RebuildPartition produced unexpected action=[" + action +
                "]");
  } else if (force_s == "RebuildAll") {
    Require(action == "RebuildAll",
            "forced RebuildAll produced unexpected action=[" + action + "]");
  }
  std::cerr << "maintenance_action=" << action << " force=" << force_s << '\n';
}

std::string AuxValueText(const core::AuxValue &value) {
  if (value.is_null) {
    return "NULL";
  }
  return std::visit(
      [](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        std::ostringstream text;
        if constexpr (std::is_same_v<Payload, double>) {
          text << std::fixed << std::setprecision(6) << payload;
        } else if constexpr (std::is_same_v<Payload, int64_t> ||
                             std::is_same_v<Payload, uint64_t>) {
          text << payload << ".000000";
        } else if constexpr (std::is_same_v<Payload, core::ScaledDecimal> ||
                             std::is_same_v<Payload, core::ScaledDecimal128>) {
          auto digits = core::ScaledDecimal128ToString(payload.value, payload.scale);
          const auto point = digits.find('.');
          if (point == std::string::npos) {
            digits += ".000000";
          } else if (digits.size() - point - 1 < 6) {
            digits.append(6 - (digits.size() - point - 1), '0');
          }
          text << digits;
        } else if constexpr (std::is_same_v<Payload, core::AuxDate>) {
          duckdb::date_t date{};
          date.days = payload.days_since_epoch;
          text << duckdb::Date::ToString(date);
        } else {
          text << payload;
        }
        return text.str();
      },
      value.payload);
}

std::string NormalizeDatabaseValue(const duckdb::Value &value) {
  if (value.IsNull()) {
    return "NULL";
  }
  if (value.type().id() == duckdb::LogicalTypeId::DATE) {
    const auto date = value.GetValue<duckdb::date_t>();
    return duckdb::Date::ToString(date);
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

bool ValuesApproxEqual(const std::string &expected, const std::string &actual) {
  if (expected == actual) {
    return true;
  }
  if (expected == "NULL" || actual == "NULL") {
    return expected == actual;
  }
  const auto beyond_double = [](const std::string &text) { return text.size() > 29; };
  if (beyond_double(expected) || beyond_double(actual)) { // more digits than any double: exact decimals
    return false;
  }
  try {
    const long double lhs = std::stold(expected);
    const long double rhs = std::stold(actual);
    const long double scale =
        std::max<long double>({1.0L, std::fabs(lhs), std::fabs(rhs)});
    return std::fabs(lhs - rhs) <= scale * 1e-6L;
  } catch (...) {
    return false;
  }
}

std::string RowKey(duckdb::MaterializedQueryResult &base, duckdb::idx_t row) {
  std::ostringstream key;
  for (duckdb::idx_t column = 0; column < base.ColumnCount(); ++column) {
    if (column != 0) {
      key << '\t';
    }
    key << NormalizeDatabaseValue(base.GetValue(column, row));
  }
  return key.str();
}

std::string RowKey(const core::AuxRow &row) {
  std::ostringstream key;
  for (std::size_t column = 0; column < row.values.size(); ++column) {
    if (column != 0) {
      key << '\t';
    }
    key << AuxValueText(row.values[column]);
  }
  return key.str();
}

// Positional mismatch: re-check tie-aware when the leading ORDER BY key is a
// floating-point column (eval/harness/float_order_ties.hpp).
std::string TieAwareMismatch(duckdb::MaterializedQueryResult &base,
                             const std::vector<core::AuxRow> &accelerated,
                             const std::string &sql, std::string *key_column) {
  namespace ties = accelerator_codegen::float_order_ties;
  std::vector<bool> is_float;
  for (const auto &type : base.types) {
    is_float.push_back(type.id() == duckdb::LogicalTypeId::DOUBLE ||
                       type.id() == duckdb::LogicalTypeId::FLOAT);
  }
  const auto key = ties::FindFloatOrderKey(sql, base.names, is_float);
  if (!key.present) {
    return "no floating-point leading ORDER BY key";
  }
  *key_column = std::to_string(key.column);
  const auto column = static_cast<duckdb::idx_t>(key.column);
  return ties::TieAwareMismatch(
      base.RowCount(), key, 1e-6, is_float,
      [&](std::size_t row) -> std::optional<double> {
        const auto value = base.GetValue(column, row);
        if (value.IsNull()) return std::nullopt;
        return value.GetValue<double>();
      },
      [&](std::size_t row) -> std::optional<double> {
        const auto &value = accelerated[row].values[column];
        if (value.is_null || !std::holds_alternative<double>(value.payload)) return std::nullopt;
        return std::get<double>(value.payload);
      },
      [&](std::size_t row) {
        std::vector<std::string> cells;
        for (duckdb::idx_t c = 0; c < base.ColumnCount(); ++c) {
          cells.push_back(NormalizeDatabaseValue(base.GetValue(c, row)));
        }
        return cells;
      },
      [&](std::size_t row) {
        std::vector<std::string> cells;
        for (const auto &value : accelerated[row].values) cells.push_back(AuxValueText(value));
        return cells;
      },
      [](std::size_t, const std::string &e, const std::string &a) { return ValuesApproxEqual(e, a); },
      &std::cerr);
}

// End of the run starting at the first mismatching `row` in which the
// expected rows are equal on all columns that match at `row` and the actual
// rows are the same rows in another order; `row` itself if there is none.
duckdb::idx_t ExactTieRunEnd(duckdb::MaterializedQueryResult &base,
                             const std::vector<core::AuxRow> &accelerated, duckdb::idx_t row) {
  std::vector<duckdb::idx_t> tied;
  for (duckdb::idx_t column = 0; column < base.ColumnCount(); ++column) {
    if (ValuesApproxEqual(NormalizeDatabaseValue(base.GetValue(column, row)),
                          AuxValueText(accelerated[row].values[column]))) {
      tied.push_back(column);
    }
  }
  duckdb::idx_t end = row + 1;
  while (end < base.RowCount()) {
    bool same = true;
    for (const auto column : tied) {
      same = same && NormalizeDatabaseValue(base.GetValue(column, end)) ==
                         NormalizeDatabaseValue(base.GetValue(column, row));
    }
    if (!same) break;
    ++end;
  }
  std::vector<std::string> expected_keys, actual_keys;
  for (duckdb::idx_t r = row; r < end; ++r) {
    expected_keys.push_back(RowKey(base, r));
    actual_keys.push_back(RowKey(accelerated[r]));
  }
  std::sort(expected_keys.begin(), expected_keys.end());
  std::sort(actual_keys.begin(), actual_keys.end());
  return expected_keys == actual_keys ? end : row;
}

bool KeysApproxEqual(const std::vector<std::string> &expected, const std::vector<std::string> &actual) {
  if (expected.size() != actual.size()) {
    return false;
  }
  const auto split = [](const std::string &key) {
    std::vector<std::string> cells;
    std::istringstream in(key);
    for (std::string cell; std::getline(in, cell, '\t');) {
      cells.push_back(cell);
    }
    return cells;
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (expected[i] == actual[i]) {
      continue;
    }
    const auto e = split(expected[i]), a = split(actual[i]);
    if (e.size() != a.size()) {
      return false;
    }
    for (std::size_t c = 0; c < e.size(); ++c) {
      if (!ValuesApproxEqual(e[c], a[c])) {
        return false;
      }
    }
  }
  return true;
}

void CompareRows(duckdb::MaterializedQueryResult &base,
                 const std::vector<core::AuxRow> &accelerated,
                 const SharedQueryCatalogEntry &entry,
                 const std::string &sql) {
  Require(base.RowCount() == accelerated.size(),
          "DuckDB differential row-count mismatch for Q" +
              std::to_string(entry.query_id) + " expected=" +
              std::to_string(base.RowCount()) + " actual=" +
              std::to_string(accelerated.size()));

  if (entry.compare_unordered) {
    std::vector<std::string> expected_keys;
    std::vector<std::string> actual_keys;
    expected_keys.reserve(base.RowCount());
    actual_keys.reserve(accelerated.size());
    for (duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
      expected_keys.push_back(RowKey(base, row));
    }
    for (const auto &row : accelerated) {
      actual_keys.push_back(RowKey(row));
    }
    std::sort(expected_keys.begin(), expected_keys.end());
    std::sort(actual_keys.begin(), actual_keys.end());
    // Rows sorted by their text, compared cell by cell within the float
    // tolerance (a float sum differs in its last digits with the summation
    // order); keys that start with exact columns keep the pairs aligned.
    Require(KeysApproxEqual(expected_keys, actual_keys),
            "DuckDB differential unordered row mismatch for Q" +
                std::to_string(entry.query_id));
    return;
  }

  for (duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
    Require(base.ColumnCount() == accelerated[row].values.size(),
            "DuckDB differential column-count mismatch");
  }
  for (duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
    for (duckdb::idx_t column = 0; column < base.ColumnCount(); ++column) {
      const auto expected = NormalizeDatabaseValue(base.GetValue(column, row));
      const auto actual = AuxValueText(accelerated[row].values[column]);
      if (ValuesApproxEqual(expected, actual)) {
        continue;
      }
      std::string message = "DuckDB differential value mismatch for Q" +
                             std::to_string(entry.query_id) + " at row " +
                             std::to_string(row) + ", column " + std::to_string(column) +
                             " expected=" + expected + " actual=" + actual;
      std::string key_column = "-";
      const auto why = TieAwareMismatch(base, accelerated, sql, &key_column);
      if (!why.empty()) {
        // Rows that agree on every column the two results agree on at this row
        // are exact ties under the ORDER BY: SQL leaves their order open, so
        // the run only has to hold the same rows.
        const auto tied_end = ExactTieRunEnd(base, accelerated, row);
        Require(tied_end > row + 1, message + " (tie-aware recheck on order key column " +
                                        key_column + ": " + why + ")");
        row = tied_end - 1;
      }
      break;
    }
  }
}

} // namespace

void CompareAuxRowsToBase(duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                          const core::QueryArguments &arguments, const std::vector<core::AuxRow> &accelerated) {
  CompareAuxRowsToSql(connection, entry, BindArguments(entry.base_sql, arguments), accelerated);
}

void CompareAuxRowsToBase(duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated) {
  CompareAuxRowsToBase(connection, entry, DefaultArguments(entry), accelerated);
}

void CompareAuxRowsToSql(duckdb::Connection &connection,
                         const SharedQueryCatalogEntry &entry,
                         const std::string &base_sql,
                         const std::vector<core::AuxRow> &accelerated) {
  auto base_result = connection.Query(base_sql);
  Require(base_result != nullptr && !base_result->HasError(),
          base_result ? base_result->GetError()
                      : "DuckDB differential query failed");
  auto &base = base_result->Cast<duckdb::MaterializedQueryResult>();
  CompareRows(base, accelerated, entry, base_sql);
}

void RequireSharedMatchesBase(duckdb::Connection &connection, DuckdbRuntime &runtime,
                              const SharedQueryCatalogEntry &entry, const core::QueryArguments &arguments) {
  CompareAuxRowsToBase(connection, entry, arguments, runtime.ExecuteCollect(arguments));
}

void RequireSharedMatchesBase(duckdb::Connection &connection,
                              DuckdbRuntime &runtime,
                              const SharedQueryCatalogEntry &entry) {
  RequireSharedMatchesBase(connection, runtime, entry, DefaultArguments(entry));
}

DifferentialSession::DifferentialSession(const std::string &db_path, const SharedQueryCatalogEntry &entry_p)
    : DifferentialSession(db_path, entry_p, DefaultArguments(entry_p)) {}

DifferentialSession::DifferentialSession(const std::string &db_path, const SharedQueryCatalogEntry &entry_p,
                                         core::QueryArguments arguments_p)
    : entry(entry_p), arguments(std::move(arguments_p)), database(db_path.c_str()), connection(database),
      runtime(connection, entry_p, 10.0) {}

void CheckReadOnly(DifferentialSession &session) {
  RequireSharedMatchesBase(session.connection, session.runtime, session.entry, session.arguments);
}

void CheckIncrementalWrite(DifferentialSession &session) {
  auto &[entry, arguments, database, connection, runtime] = session;
  const auto before_fp = FingerprintSharedBaseSql(connection, entry, arguments);
  auto probe = FindInvalidatingSharedProbe(connection, entry, arguments);
  Require(SharedProbeChangesBaseResult(connection, entry, arguments, probe, before_fp),
          "selected probe no longer changes DuckDB result for Q" +
              std::to_string(entry.query_id));
  runtime.Apply(probe);
  RequireForcedMaintenanceAction(runtime);
  const auto after_fp = FingerprintSharedBaseSql(connection, entry, arguments);
  Require(after_fp != before_fp,
          "Apply() did not change DuckDB Q" + std::to_string(entry.query_id) +
              " result — write did not invalidate observable query state");
  RequireSharedMatchesBase(connection, runtime, entry, arguments);
  runtime.ApplyWrite(probe.restore_sql);
  RequireSharedMatchesBase(connection, runtime, entry, arguments);
}

void RunDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry,
                          const core::QueryArguments &arguments) {
  DifferentialSession session(db_path, entry, arguments);
  session.runtime.Build();
  CheckReadOnly(session);
}

void RunDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry) {
  RunDifferentialCheck(db_path, entry, DefaultArguments(entry));
}

void RunIncrementalDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry,
                                     const core::QueryArguments &arguments) {
  DifferentialSession session(db_path, entry, arguments);
  session.runtime.Build();
  CheckReadOnly(session);
  CheckIncrementalWrite(session);
}

void RunIncrementalDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry) {
  RunIncrementalDifferentialCheck(db_path, entry, DefaultArguments(entry));
}

void CheckRfChunks(DifferentialSession &session, int num_chunks, int order_count) {
  auto &[entry, arguments, database, connection, runtime] = session;
  Require(std::string(entry.workload_name) == "tpch",
          "RF differential is TPC-H only");
  Require(entry.query_id != 11,
          "Q11 is unaffected by RF (no orders/lineitem dependency)");
  Require(num_chunks > 0, "num_chunks must be positive");
  Require(order_count > 0, "order_count must be positive");

  const auto fallbacks_before = runtime.Stats().reads_fallback;
  const auto before_fp = FingerprintSharedBaseSql(connection, entry, arguments);
  bool changed = false;
  int orders = order_count;
  int seq = 0;
  // Apply RF chunks (escalating size) until DuckDB's answer moves when possible.
  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts && !changed; ++attempt) {
    ApplyTpchRfChunk(connection, runtime, seq++, orders);
    try {
      RequireSharedMatchesBase(connection, runtime, entry, arguments);
    } catch (const std::exception &ex) {
      throw std::runtime_error(std::string(ex.what()) + " (after RF chunk seq=" +
                               std::to_string(seq - 1) + " order_count=" +
                               std::to_string(orders) + ")");
    }
    if (FingerprintSharedBaseSql(connection, entry, arguments) != before_fp) {
      changed = true;
      break;
    }
    orders = std::min(std::max(orders * 2, orders + 10), 200);
  }

  if (!changed) {
    // Standard TPC-H RF date ranges often miss historical predicates (e.g. Q3
    // TOP-10). Still require a write that invalidates this query: apply a
    // result-changing shared probe after RF maintenance has run.
    auto probe = FindInvalidatingSharedProbe(connection, entry, arguments);
    runtime.Apply(probe);
    const auto after_fp = FingerprintSharedBaseSql(connection, entry, arguments);
    Require(after_fp != before_fp,
            "RF+probe did not change DuckDB Q" + std::to_string(entry.query_id) +
                " result — cannot confirm aux invalidation");
    RequireSharedMatchesBase(connection, runtime, entry, arguments);
    changed = true;
  }

  // Additional RF chunks (if requested) must stay correct after invalidation.
  while (seq < num_chunks) {
    ApplyTpchRfChunk(connection, runtime, seq++, order_count);
    try {
      RequireSharedMatchesBase(connection, runtime, entry, arguments);
    } catch (const std::exception &ex) {
      throw std::runtime_error(std::string(ex.what()) + " (after RF chunk seq=" +
                               std::to_string(seq - 1) + " order_count=" +
                               std::to_string(order_count) + ")");
    }
  }
  Require(changed, "internal: RF differential exited without invalidation");
  // Maintained incrementally, the state serves every read: none falls back.
  const auto fallbacks = runtime.Stats().reads_fallback - fallbacks_before;
  Require(fallbacks == 0,
          "Q" + std::to_string(entry.query_id) + ": " + std::to_string(fallbacks) +
              " reads fell back to SQL in steady state");
}

void RunRfChunkDifferentialCheck(const std::string &db_path,
                                 const SharedQueryCatalogEntry &entry,
                                 int num_chunks, int order_count) {
  DifferentialSession session(db_path, entry);
  session.runtime.Build();
  CheckReadOnly(session);
  CheckRfChunks(session, num_chunks, order_count);
}

void CheckNoDrift(DifferentialSession &session, const std::function<suite::Rows()> &answers) {
  const auto maintained = answers();
  session.runtime.Build();
  RequireSharedMatchesBase(session.connection, session.runtime, session.entry, session.arguments);
  Require(suite::SameRows(answers(), maintained),
          "Q" + std::to_string(session.entry.query_id) +
              ": the maintained state answers differently from one built from scratch on the same data");
}

void CheckNoDrift(DifferentialSession &session) {
  CheckNoDrift(session, [&] {
    suite::Rows rows;
    for (const auto &row : session.runtime.ExecuteCollect(session.arguments)) {
      rows.emplace_back();
      for (const auto &value : row.values)
        rows.back().push_back(AuxValueText(value));
    }
    return rows;
  });
}

namespace {

std::vector<int64_t> KeyList(duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  Require(result != nullptr && !result->HasError(), result ? result->GetError() : sql);
  std::vector<int64_t> keys;
  for (duckdb::idx_t i = 0; i < result->RowCount(); ++i)
    keys.push_back(result->GetValue(0, i).GetValue<int64_t>());
  return keys;
}

// TPC-H writes of every kind an accelerator must maintain under: RF2 / RF1
// (orders with their lineitems, at both ends of the key range and in the
// middle), lineitem and customer updates, and parts / suppliers deleted and
// inserted with their partsupp rows, plus a supplier-only update.
std::vector<NamedWrite> TpchDifferentialWrites(duckdb::Connection &connection, int rounds) {
  const auto n = std::to_string(rounds + 2);
  const auto low_orders = KeyList(connection, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n);
  const auto mid_orders = KeyList(connection, "SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT " + n +
                                                  " OFFSET (SELECT count(*) // 2 FROM orders)");
  const auto top_orders = KeyList(connection, "SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT " + n);
  const auto low_parts = KeyList(connection, "SELECT p_partkey FROM part ORDER BY p_partkey LIMIT " + n);
  const auto top_parts = KeyList(connection, "SELECT p_partkey FROM part ORDER BY p_partkey DESC LIMIT " + n);
  const auto low_supps = KeyList(connection, "SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT " + n);
  const auto top_supps = KeyList(connection, "SELECT s_suppkey FROM supplier ORDER BY s_suppkey DESC LIMIT " + n);
  const auto mid_custs = KeyList(connection, "SELECT c_custkey FROM customer ORDER BY c_custkey LIMIT " + n +
                                                 " OFFSET (SELECT count(*) // 3 FROM customer)");
  int64_t next_order = KeyList(connection, "SELECT max(o_orderkey) FROM orders")[0] + 1;
  int64_t next_part = KeyList(connection, "SELECT max(p_partkey) FROM part")[0] + 1;
  int64_t next_supp = KeyList(connection, "SELECT max(s_suppkey) FROM supplier")[0] + 1;
  const char *segments[] = {"BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"};
  auto s = [](int64_t v) { return std::to_string(v); };
  auto delete_order = [&](int64_t k) {
    return "DELETE FROM lineitem WHERE l_orderkey = " + s(k) + "; DELETE FROM orders WHERE o_orderkey = " + s(k);
  };
  std::vector<NamedWrite> writes;
  for (int r = 0; r < rounds; ++r) {
    const auto tag = "#" + std::to_string(r);
    writes.push_back({"rf2 low order" + tag, delete_order(low_orders[r])});
    writes.push_back({"rf2 mid order" + tag, delete_order(mid_orders[r])});
    const auto k = next_order++;
    writes.push_back({"rf1 order" + tag,
                      "INSERT INTO orders SELECT * REPLACE (" + s(k) + " AS o_orderkey) FROM orders WHERE o_orderkey = " +
                          s(top_orders[r]) + "; INSERT INTO lineitem SELECT * REPLACE (" + s(k) +
                          " AS l_orderkey) FROM lineitem WHERE l_orderkey = " + s(top_orders[r])});
    writes.push_back({"lineitem update" + tag,
                      "UPDATE lineitem SET l_quantity = l_quantity + 1, l_discount = 0.04 WHERE l_orderkey = " +
                          s(mid_orders[r + 1])});
    writes.push_back({"customer segment" + tag, std::string("UPDATE customer SET c_mktsegment = '") +
                                                    segments[r % 5] + "' WHERE c_custkey = " + s(mid_custs[r])});
    writes.push_back({"part delete" + tag, "DELETE FROM partsupp WHERE ps_partkey = " + s(low_parts[r]) +
                                               "; DELETE FROM part WHERE p_partkey = " + s(low_parts[r])});
    const auto p = next_part++;
    writes.push_back({"part insert" + tag,
                      "INSERT INTO part SELECT * REPLACE (" + s(p) + " AS p_partkey) FROM part WHERE p_partkey = " +
                          s(top_parts[r]) + "; INSERT INTO partsupp SELECT * REPLACE (" + s(p) +
                          " AS ps_partkey) FROM partsupp WHERE ps_partkey = " + s(top_parts[r])});
    writes.push_back({"supplier delete" + tag, "DELETE FROM partsupp WHERE ps_suppkey = " + s(low_supps[r]) +
                                                   "; DELETE FROM supplier WHERE s_suppkey = " + s(low_supps[r])});
    const auto q = next_supp++;
    writes.push_back({"supplier insert" + tag,
                      "INSERT INTO supplier SELECT * REPLACE (" + s(q) +
                          " AS s_suppkey) FROM supplier WHERE s_suppkey = " + s(top_supps[r]) +
                          "; INSERT INTO partsupp SELECT * REPLACE (" + s(q) +
                          " AS ps_suppkey) FROM partsupp WHERE ps_suppkey = " + s(top_supps[r])});
    writes.push_back({"supplier rename" + tag, "UPDATE supplier SET s_name = s_name || 'x', s_phone = '10-" +
                                                   std::to_string(100 + r) + "-000-0000'"});
  }
  return writes;
}

// Writes aimed at one query's answer (its catalog default arguments): they
// change rows the answer depends on, in the ways its maintenance must handle.
std::vector<NamedWrite> TargetedWrites(duckdb::Connection &connection, const SharedQueryCatalogEntry &entry) {
  std::vector<NamedWrite> writes;
  auto list = [](const std::vector<int64_t> &keys) {
    std::string out;
    for (const auto k : keys)
      out += (out.empty() ? "" : ",") + std::to_string(k);
    return out;
  };
  switch (entry.query_id) {
  case 3: { // customers of current top-10 orders change segment / are deleted / re-inserted (BUILDING)
    const auto custs = KeyList(connection,
        std::string("SELECT o_custkey FROM orders WHERE o_orderkey IN (SELECT l_orderkey FROM (") + entry.base_sql +
        ")) GROUP BY o_custkey ORDER BY min(o_orderkey) LIMIT 3");
    if (custs.size() == 3) {
      const auto c0 = std::to_string(custs[0]), c1 = std::to_string(custs[1]), c2 = std::to_string(custs[2]);
      writes.push_back({"q3 top customer to MACHINERY (acctbal too)",
                        "UPDATE customer SET c_acctbal = c_acctbal + 1, c_mktsegment = 'MACHINERY' WHERE c_custkey = " + c0});
      writes.push_back({"q3 top customer back to BUILDING",
                        "UPDATE customer SET c_mktsegment = 'BUILDING' WHERE c_custkey = " + c0});
      writes.push_back({"q3 acctbal only", "UPDATE customer SET c_acctbal = c_acctbal + 1 WHERE c_custkey = " + c0});
      writes.push_back({"q3 top customer deleted (orders stay)",
                        "CREATE OR REPLACE TEMP TABLE q3_keep AS SELECT * FROM customer WHERE c_custkey = " + c1 +
                            "; DELETE FROM customer WHERE c_custkey = " + c1});
      writes.push_back({"q3 deleted customer re-inserted", "INSERT INTO customer SELECT * FROM q3_keep"});
      writes.push_back({"q3 customer segment + lineitem delete in one transaction",
                        "UPDATE customer SET c_mktsegment = 'FURNITURE' WHERE c_custkey = " + c2 +
                            "; DELETE FROM lineitem WHERE rowid IN (SELECT l.rowid FROM lineitem l JOIN orders o ON "
                            "o.o_orderkey = l.l_orderkey WHERE o.o_custkey = " + c2 + " LIMIT 1)"});
      writes.push_back({"q3 customer back to BUILDING (again)",
                        "UPDATE customer SET c_mktsegment = 'BUILDING' WHERE c_custkey = " + c2});
    }
    break;
  }
  case 8: { // orders changed or deleted alone while their lineitems stay (INDIA / ASIA / PROMO BRUSHED BRASS)
    const auto orders = KeyList(connection,
        "SELECT o_orderkey FROM orders JOIN lineitem ON l_orderkey = o_orderkey JOIN part ON p_partkey = l_partkey "
        "JOIN customer ON c_custkey = o_custkey JOIN nation ON n_nationkey = c_nationkey JOIN region ON "
        "r_regionkey = n_regionkey WHERE r_name = 'ASIA' AND p_type = 'PROMO BRUSHED BRASS' AND o_orderdate "
        "BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' GROUP BY o_orderkey ORDER BY o_orderkey LIMIT 3");
    const auto other_cust = KeyList(connection,
        "SELECT c_custkey FROM customer JOIN nation ON n_nationkey = c_nationkey JOIN region ON r_regionkey = "
        "n_regionkey WHERE r_name <> 'ASIA' ORDER BY c_custkey LIMIT 1");
    if (orders.size() == 3 && other_cust.size() == 1) {
      const auto o0 = std::to_string(orders[0]), o1 = std::to_string(orders[1]), o2 = std::to_string(orders[2]);
      writes.push_back({"q8 order date moves year",
                        "UPDATE orders SET o_orderdate = CASE WHEN year(o_orderdate) = 1995 THEN o_orderdate + "
                        "INTERVAL 1 YEAR ELSE o_orderdate - INTERVAL 1 YEAR END WHERE o_orderkey = " + o0});
      writes.push_back({"q8 order moves customer (out of the region)",
                        "UPDATE orders SET o_custkey = " + std::to_string(other_cust[0]) + " WHERE o_orderkey = " + o1});
      writes.push_back({"q8 order deleted alone (lineitems stay)", "DELETE FROM orders WHERE o_orderkey = " + o2});
    }
    break;
  }
  case 7: { // dimension changes under lineitems in the window (UNITED KINGDOM / BRAZIL)
    const std::string window = " JOIN lineitem ON l_suppkey = s_suppkey JOIN orders ON o_orderkey = l_orderkey "
                               "JOIN customer ON c_custkey = o_custkey WHERE l_shipdate BETWEEN DATE '1995-01-01' "
                               "AND DATE '1996-12-31' AND ";
    const auto supp = KeyList(connection, "SELECT s_suppkey FROM supplier" + window +
                                              "s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'UNITED KINGDOM') "
                                              "AND c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL') "
                                              "GROUP BY s_suppkey ORDER BY s_suppkey LIMIT 2");
    const auto cust = KeyList(connection, "SELECT c_custkey FROM supplier" + window +
                                              "c_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL') "
                                              "AND s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'UNITED KINGDOM') "
                                              "GROUP BY c_custkey ORDER BY c_custkey LIMIT 2");
    if (supp.size() == 2 && cust.size() == 2) {
      const auto s0 = std::to_string(supp[0]), s1 = std::to_string(supp[1]);
      const auto c0 = std::to_string(cust[0]), c1 = std::to_string(cust[1]);
      const std::string germany = "(SELECT n_nationkey FROM nation WHERE n_name = 'BRAZIL')";
      const std::string france = "(SELECT n_nationkey FROM nation WHERE n_name = 'UNITED KINGDOM')";
      writes.push_back({"q7 supplier to BRAZIL", "UPDATE supplier SET s_nationkey = " + germany + " WHERE s_suppkey = " + s0});
      writes.push_back({"q7 supplier back to UNITED KINGDOM", "UPDATE supplier SET s_nationkey = " + france + " WHERE s_suppkey = " + s0});
      writes.push_back({"q7 customer to UNITED KINGDOM", "UPDATE customer SET c_nationkey = " + france + " WHERE c_custkey = " + c0});
      writes.push_back({"q7 customer delete (orders stay)", "DELETE FROM customer WHERE c_custkey = " + c1});
      writes.push_back({"q7 supplier delete (lineitems stay)", "DELETE FROM supplier WHERE s_suppkey = " + s1});
    }
    break;
  }
  case 12: { // flip orders between the high and low priority classes (orders with counted lineitems)
    const std::string counted = "SELECT o_orderkey FROM orders JOIN lineitem ON l_orderkey = o_orderkey "
                                "WHERE l_shipmode IN ('MAIL', 'SHIP') AND l_commitdate < l_receiptdate "
                                "AND l_shipdate < l_commitdate AND l_receiptdate >= DATE '1995-01-01' "
                                "AND l_receiptdate < DATE '1996-01-01' AND o_orderpriority IN ";
    const auto high = KeyList(connection, counted + "('1-URGENT', '2-HIGH') GROUP BY o_orderkey ORDER BY o_orderkey LIMIT 2");
    const auto low = KeyList(connection, counted + "('3-MEDIUM', '5-LOW') GROUP BY o_orderkey ORDER BY o_orderkey LIMIT 2");
    if (high.size() == 2 && low.size() == 2) {
      const auto h0 = std::to_string(high[0]), h1 = std::to_string(high[1]);
      const auto l0 = std::to_string(low[0]), l1 = std::to_string(low[1]);
      writes.push_back({"q12 flip high->low", "UPDATE orders SET o_orderpriority = '5-LOW' WHERE o_orderkey = " + h0});
      writes.push_back({"q12 flip low->high", "UPDATE orders SET o_orderpriority = '1-URGENT' WHERE o_orderkey = " + l0});
      writes.push_back({"q12 same class", "UPDATE orders SET o_orderpriority = '2-HIGH' WHERE o_orderkey = " + l0});
      // a flip with lineitem changes of the same order in the same transaction
      writes.push_back({"q12 flip + lineitem delete",
                        "UPDATE orders SET o_orderpriority = '4-NOT SPECIFIED' WHERE o_orderkey = " + h1 +
                            "; DELETE FROM lineitem WHERE l_orderkey = " + h1 + " AND l_linenumber = (SELECT min(l_linenumber) "
                            "FROM lineitem WHERE l_orderkey = " + h1 + ")"});
      writes.push_back({"q12 flip + lineitem insert",
                        "INSERT INTO lineitem SELECT * REPLACE (l_linenumber + 100 AS l_linenumber) FROM lineitem "
                        "WHERE l_orderkey = " + l1 + " AND l_shipmode IN ('MAIL', 'SHIP') AND l_commitdate < l_receiptdate "
                        "AND l_shipdate < l_commitdate LIMIT 1; UPDATE orders SET o_orderpriority = '1-URGENT' "
                        "WHERE o_orderkey = " + l1});
    }
    break;
  }
  case 15: { // rename the current winner(s): a supplier-only write
    const auto winners = KeyList(connection, std::string("SELECT * FROM (") + entry.base_sql + ")");
    if (!winners.empty())
      writes.push_back({"rename q15 winner", "UPDATE supplier SET s_name = 'Supplier#renamed', s_address = 'moved', "
                                             "s_phone = '00-000-000-0000' WHERE s_suppkey IN (" + list(winners) + ")"});
    break;
  }
  case 16: // a complaint supplier deleted, its partsupp rows left: they now count
    writes.push_back({"delete complaint supplier (partsupp kept)",
                      "DELETE FROM supplier WHERE s_suppkey = (SELECT min(s_suppkey) FROM supplier "
                      "WHERE s_comment LIKE '%Customer%Complaints%')"});
    break;
  case 19: { // qualifying lineitems deleted, then a qualifying part deleted (its lineitems kept)
    const std::string branch = "p_brand = 'Brand#31' AND p_container IN ('SM CASE','SM BOX','SM PACK','SM PKG') "
                               "AND l_quantity BETWEEN 4 AND 14 AND p_size BETWEEN 1 AND 5 "
                               "AND l_shipmode IN ('AIR','AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON'";
    writes.push_back({"delete q19 qualifying lineitems",
                      "DELETE FROM lineitem WHERE rowid IN (SELECT l.rowid FROM lineitem l JOIN part p ON "
                      "p.p_partkey = l.l_partkey WHERE " + branch + " LIMIT 5)"});
    const auto part = KeyList(connection, "SELECT p_partkey FROM lineitem JOIN part ON p_partkey = l_partkey WHERE " +
                                              branch + " LIMIT 1");
    if (!part.empty())
      writes.push_back({"delete q19 qualifying part", "DELETE FROM part WHERE p_partkey = " + std::to_string(part[0])});
    break;
  }
  case 20: { // every lineitem of a few qualifying (part, supplier) pairs deleted
    auto pairs = connection.Query(
        "SELECT DISTINCT l_partkey, l_suppkey FROM lineitem JOIN part ON p_partkey = l_partkey "
        "JOIN supplier ON s_suppkey = l_suppkey JOIN nation ON n_nationkey = s_nationkey "
        "WHERE p_name LIKE 'gainsboro%' AND n_name = 'RUSSIA' AND l_shipdate >= DATE '1997-01-01' "
        "AND l_shipdate < DATE '1998-01-01' ORDER BY 1, 2 LIMIT 3");
    Require(pairs && !pairs->HasError(), pairs ? pairs->GetError() : "q20 pairs");
    for (duckdb::idx_t i = 0; i < pairs->RowCount(); ++i)
      writes.push_back({"delete q20 pair lineitems",
                        "DELETE FROM lineitem WHERE l_partkey = " + pairs->GetValue(0, i).ToString() +
                            " AND l_suppkey = " + pairs->GetValue(1, i).ToString()});
    break;
  }
  default:
    break;
  }
  return writes;
}

} // namespace

std::vector<NamedWrite> TpchSqlWrites(duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                                      int rounds) {
  auto writes = TargetedWrites(connection, entry);
  for (auto &w : TpchDifferentialWrites(connection, rounds))
    writes.push_back(std::move(w));
  return writes;
}

void RunSqlWritesDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry, int rounds) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  DuckdbRuntime runtime(connection, entry, 10.0);
  runtime.Build();
  RequireSharedMatchesBase(connection, runtime, entry);
  const auto writes = TpchSqlWrites(connection, entry, rounds);
  for (const auto &write : writes) {
    runtime.ApplyWrite(write.sql);
    LogMaintenanceAction(runtime, write.name);
    try {
      RequireSharedMatchesBase(connection, runtime, entry);
    } catch (const std::exception &ex) {
      throw std::runtime_error(std::string(ex.what()) + " (after " + write.name + ")");
    }
  }
}

void RunBulkWritesDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  DuckdbRuntime runtime(connection, entry, 10.0);
  runtime.Build();
  RequireSharedMatchesBase(connection, runtime, entry);
  const std::string customers = "(SELECT max(c_custkey) FROM customer)";
  // {name, apply, undo}: every write is undone by its second statement(s).
  const std::vector<std::array<std::string, 3>> writes = {
      {"lineitem update, every row", "UPDATE lineitem SET l_quantity = l_quantity + 1",
       "UPDATE lineitem SET l_quantity = l_quantity - 1"},
      {"orders and lineitems deleted, 25%",
       "CREATE OR REPLACE TABLE bulk_orders AS SELECT * FROM orders WHERE o_orderkey % 4 = 0; "
       "CREATE OR REPLACE TABLE bulk_lineitem AS SELECT * FROM lineitem WHERE l_orderkey % 4 = 0; "
       "DELETE FROM lineitem WHERE l_orderkey % 4 = 0; DELETE FROM orders WHERE o_orderkey % 4 = 0",
       "INSERT INTO orders SELECT * FROM bulk_orders; INSERT INTO lineitem SELECT * FROM bulk_lineitem"},
      {"orders customer moved, every row",
       "UPDATE orders SET o_custkey = o_custkey % " + customers + " + 1",
       "UPDATE orders SET o_custkey = (o_custkey + " + customers + " - 2) % " + customers + " + 1"},
      {"partsupp deleted, 10%",
       "CREATE OR REPLACE TABLE bulk_partsupp AS SELECT * FROM partsupp WHERE ps_partkey % 10 = 0; "
       "DELETE FROM partsupp WHERE ps_partkey % 10 = 0",
       "INSERT INTO partsupp SELECT * FROM bulk_partsupp"},
      {"part update, every row", "UPDATE part SET p_size = p_size + 1", "UPDATE part SET p_size = p_size - 1"},
      {"supplier renamed, every row", "UPDATE supplier SET s_name = s_name || 'x'",
       "UPDATE supplier SET s_name = left(s_name, length(s_name) - 1)"},
      {"customer nation moved, 1%",
       "UPDATE customer SET c_nationkey = (c_nationkey + 1) % 25 WHERE c_custkey % 100 = 0",
       "UPDATE customer SET c_nationkey = (c_nationkey + 24) % 25 WHERE c_custkey % 100 = 0"},
      {"nation renamed", "UPDATE nation SET n_name = n_name || 'X'",
       "UPDATE nation SET n_name = left(n_name, length(n_name) - 1)"},
      {"region renamed", "UPDATE region SET r_name = r_name || 'X'",
       "UPDATE region SET r_name = left(r_name, length(r_name) - 1)"},
  };
  for (const auto &[name, apply, undo] : writes) {
    for (const auto &[step, sql] : {std::pair<std::string, std::string>{"", apply}, {" undone", undo}}) {
      const auto label = name + step;
      const auto before = runtime.Stats();
      runtime.ApplyWrite(sql);
      const auto after = runtime.Stats();
      const bool maintained = after.commits_applied + after.commits_stale >
                              before.commits_applied + before.commits_stale;
      if (maintained) {
        LogMaintenanceAction(runtime, label);
      } else if (std::getenv("ACCEL_LOG_MAINTENANCE_ACTIONS")) {
        std::cerr << "maintenance_action=none after " << label << " (not a dependency)\n";
      }
      try {
        try {
          RequireSharedMatchesBase(connection, runtime, entry); // maybe mid-rebuild: fallback
        } catch (const std::exception &ex) {
          throw std::runtime_error(std::string(ex.what()) + " [first read, fallbacks=" + std::to_string(runtime.Stats().reads_fallback) + " builds=" + std::to_string(runtime.Stats().builds) + "]");
        }
        runtime.EnsureBuilt();
        const auto fallbacks = runtime.Stats().reads_fallback;
        RequireSharedMatchesBase(connection, runtime, entry);
        Require(runtime.Stats().reads_fallback == fallbacks, "the settled state did not serve the read");
      } catch (const std::exception &ex) {
        throw std::runtime_error(std::string(ex.what()) + " (after " + label + ")");
      }
    }
  }
}

AdversarialPlan AdversarialPlanFromEnv(int max_probes, int min_chunks, bool require_bulk) {
  AdversarialPlan plan{max_probes, 2, min_chunks, require_bulk};
  if (const char *env = std::getenv("ACCEL_ADV_MAX_PROBES"); env && *env) {
    plan.max_probes = std::stoi(env);
  }
  if (const char *env = std::getenv("ACCEL_ADV_MIN_CHUNKS"); env && *env) {
    plan.min_chunks = std::stoi(env);
  }
  if (const char *env = std::getenv("ACCEL_ADV_REQUIRE_BULK"); env && *env) {
    plan.require_bulk = std::string(env) != "0";
  }
  if (const char *env = std::getenv("ACCEL_ADV_MIN_PROBES"); env && *env) {
    plan.min_probes = std::stoi(env);
  }
  Require(plan.max_probes >= 2, "adversarial max_probes must be >= 2");
  Require(plan.max_probes <= 16, "adversarial max_probes must be <= 16");
  Require(plan.min_probes >= 2 && plan.min_probes <= plan.max_probes,
          "adversarial min_probes must be in [2, max_probes]");
  return plan;
}

void CheckAdversarialWrites(DifferentialSession &session, const AdversarialPlan &plan) {
  auto &[entry, arguments, database, connection, runtime] = session;
  auto probes = CollectInvalidatingSharedProbes(connection, entry, arguments, 256, plan.max_probes);
  Require(static_cast<int>(probes.size()) >= plan.min_probes,
          "adversarial needs >= " + std::to_string(plan.min_probes) +
              " invalidating catalog probes for Q" +
              std::to_string(entry.query_id) + " got=" +
              std::to_string(probes.size()));

  // Every write and its undo go through the runtime, which maintains the state for both.
  for (std::size_t i = 0; i < probes.size(); ++i) {
    auto probe = probes[i];
    const auto before_fp = FingerprintSharedBaseSql(connection, entry, arguments);
    Require(SharedProbeChangesBaseResult(connection, entry, arguments, probe, before_fp),
            "adversarial probe " + std::to_string(i) +
                " no longer changes DuckDB result for Q" +
                std::to_string(entry.query_id));
    const auto before_stats = runtime.Stats();
    runtime.Apply(probe);
    const auto after_stats = runtime.Stats();
    suite::ProbeCostsMs().push_back((after_stats.apply_us + after_stats.build_us - before_stats.apply_us -
                                     before_stats.build_us) / 1000.0);
    RequireForcedMaintenanceAction(runtime);
    const auto after_fp = FingerprintSharedBaseSql(connection, entry, arguments);
    Require(after_fp != before_fp,
            "adversarial Apply() did not invalidate DuckDB Q" +
                std::to_string(entry.query_id) + " probe=" +
                std::to_string(i));
    RequireSharedMatchesBase(connection, runtime, entry, arguments);
    runtime.ApplyWrite(probe.restore_sql);
    RequireSharedMatchesBase(connection, runtime, entry, arguments);
  }

  const std::size_t sequence_len = std::min<std::size_t>(3, probes.size());
  for (std::size_t i = 0; i < sequence_len; ++i) {
    runtime.Apply(probes[i]);
    RequireSharedMatchesBase(connection, runtime, entry, arguments);
  }
  for (std::size_t i = sequence_len; i-- > 0;) {
    runtime.ApplyWrite(probes[i].restore_sql);
  }
  RequireSharedMatchesBase(connection, runtime, entry, arguments);
}

void CheckRefreshWrites(DifferentialSession &session, const AdversarialPlan &plan) {
  if (!plan.require_bulk) {
    return;
  }
  Require(std::string(session.entry.workload_name) == "tpch" && session.entry.query_id != 11,
          "adversarial bulk required but query cannot use TPC-H RF "
          "(orders/lineitem); set bulk_writes.required=false");
  Require(plan.min_chunks >= 1,
          "adversarial min_chunks must be >= 1 when bulk required");
  CheckRfChunks(session, plan.min_chunks, 1);
}

void RunAdversarialWriteDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry,
                                          int max_probes, int min_chunks, bool require_bulk) {
  RunAdversarialWriteDifferentialCheck(db_path, entry, DefaultArguments(entry), max_probes, min_chunks, require_bulk);
}

void RunAdversarialWriteDifferentialCheck(const std::string &db_path, const SharedQueryCatalogEntry &entry,
                                          const core::QueryArguments &arguments, int max_probes, int min_chunks,
                                          bool require_bulk) {
  const auto plan = AdversarialPlanFromEnv(max_probes, min_chunks, require_bulk);
  DifferentialSession session(db_path, entry, arguments);
  session.runtime.Build();
  CheckReadOnly(session);
  CheckAdversarialWrites(session, plan);
  CheckRefreshWrites(session, plan);
  CheckNoDrift(session);
}

} // namespace accelerator_codegen::aux_state::harness
