// SCAFFOLDING
#include "accelerator_codegen/harness/differential.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::harness {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace aux_engine = ::accelerator_codegen::aux_state::engine;
using aux_engine::duckdb::DuckdbRuntime;
using aux_engine::SharedQueryCatalogEntry;
using aux_engine::SharedProbe;
using aux_engine::FindSharedProbe;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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
        } else if constexpr (std::is_same_v<Payload, core::ScaledDecimal>) {
          const bool negative = payload.value < 0;
          const uint64_t magnitude = negative
                                         ? static_cast<uint64_t>(-(payload.value + 1)) + 1
                                         : static_cast<uint64_t>(payload.value);
          auto digits = std::to_string(magnitude);
          if (payload.scale > 0) {
            if (digits.size() <= static_cast<std::size_t>(payload.scale)) {
              digits.insert(0, static_cast<std::size_t>(payload.scale) + 1 -
                                       digits.size(),
                            '0');
            }
            digits.insert(digits.size() - payload.scale, 1, '.');
          }
          if (negative) {
            digits.insert(digits.begin(), '-');
          }
          const auto decimal = digits.find('.');
          if (decimal == std::string::npos) {
            digits += ".000000";
          } else if (digits.size() - decimal - 1 < 6) {
            digits.append(6 - (digits.size() - decimal - 1), '0');
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
  try {
    const long double lhs = std::stold(expected);
    const long double rhs = std::stold(actual);
    const long double scale =
        std::max<long double>({1.0L, std::fabsl(lhs), std::fabsl(rhs)});
    return std::fabsl(lhs - rhs) <= scale * 1e-6L;
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

void CompareRows(duckdb::MaterializedQueryResult &base,
                 const std::vector<core::AuxRow> &accelerated,
                 const SharedQueryCatalogEntry &entry) {
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
    Require(expected_keys == actual_keys,
            "DuckDB differential unordered row mismatch for Q" +
                std::to_string(entry.query_id));
    return;
  }

  for (duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
    Require(base.ColumnCount() == accelerated[row].values.size(),
            "DuckDB differential column-count mismatch");
    for (duckdb::idx_t column = 0; column < base.ColumnCount(); ++column) {
      const auto expected = NormalizeDatabaseValue(base.GetValue(column, row));
      const auto actual = AuxValueText(accelerated[row].values[column]);
      Require(ValuesApproxEqual(expected, actual),
              "DuckDB differential value mismatch for Q" +
                  std::to_string(entry.query_id) + " at row " +
                  std::to_string(row) + ", column " + std::to_string(column) +
                  " expected=" + expected + " actual=" + actual);
    }
  }
}

} // namespace

void CompareAuxRowsToBase(duckdb::Connection &connection,
                          const SharedQueryCatalogEntry &entry,
                          const std::vector<core::AuxRow> &accelerated) {
  CompareAuxRowsToSql(connection, entry, entry.base_sql, accelerated);
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
  CompareRows(base, accelerated, entry);
}

void RequireSharedMatchesBase(duckdb::Connection &connection,
                              DuckdbRuntime &runtime,
                              const SharedQueryCatalogEntry &entry) {
  CompareAuxRowsToBase(connection, entry, runtime.ExecuteCollect());
}

void RunDifferentialCheck(const std::string &db_path,
                          const SharedQueryCatalogEntry &entry) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  DuckdbRuntime runtime(connection, entry, 10.0);
  runtime.Build();
  RequireSharedMatchesBase(connection, runtime, entry);
}

std::string FingerprintBaseSql(::duckdb::Connection &connection,
                               const SharedQueryCatalogEntry &entry) {
  auto base_result = connection.Query(entry.base_sql);
  Require(base_result != nullptr && !base_result->HasError(),
          base_result ? base_result->GetError()
                      : "DuckDB fingerprint query failed");
  auto &base = base_result->Cast<::duckdb::MaterializedQueryResult>();
  std::ostringstream out;
  out << "rows=" << base.RowCount() << '\n';
  for (::duckdb::idx_t row = 0; row < base.RowCount(); ++row) {
    out << RowKey(base, row) << '\n';
  }
  return out.str();
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

bool ProbeChangesBaseResult(::duckdb::Connection &connection,
                            const SharedQueryCatalogEntry &entry,
                            const SharedProbe &probe,
                            const std::string &before_fp) {
  connection.BeginTransaction();
  auto updated = connection.Query(probe.update_sql);
  if (updated == nullptr || updated->HasError()) {
    connection.Rollback();
    return false;
  }
  const auto after_fp = FingerprintBaseSql(connection, entry);
  connection.Rollback();
  return after_fp != before_fp;
}

SharedProbe FindInvalidatingSharedProbe(::duckdb::Connection &connection,
                                        const SharedQueryCatalogEntry &entry) {
  const auto before_fp = FingerprintBaseSql(connection, entry);
  const std::string probe_sql =
      ExpandProbeSelectLimit(entry.probe_select_sql, /*limit=*/256);
  auto result = connection.Query(probe_sql);
  Require(result != nullptr && !result->HasError() && result->RowCount() >= 1,
          result ? ("invalidating probe select failed for Q" +
                    std::to_string(entry.query_id) + ": " + result->GetError())
                 : "invalidating probe select returned null");
  auto &materialized = result->Cast<::duckdb::MaterializedQueryResult>();
  for (::duckdb::idx_t row = 0; row < materialized.RowCount(); ++row) {
    auto probe = entry.build_probe(materialized, row);
    if (ProbeChangesBaseResult(connection, entry, probe, before_fp)) {
      return probe;
    }
  }
  throw std::runtime_error(
      "no shared probe candidate changed DuckDB Q" +
      std::to_string(entry.query_id) +
      " result (need a write that invalidates this query's aux state)");
}

void RunIncrementalDifferentialCheck(const std::string &db_path,
                                     const SharedQueryCatalogEntry &entry) {
  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  DuckdbRuntime runtime(connection, entry, 10.0);
  runtime.Build();
  RequireSharedMatchesBase(connection, runtime, entry);

  const auto before_fp = FingerprintBaseSql(connection, entry);
  auto probe = FindInvalidatingSharedProbe(connection, entry);
  Require(ProbeChangesBaseResult(connection, entry, probe, before_fp),
          "selected probe no longer changes DuckDB result for Q" +
              std::to_string(entry.query_id));

  runtime.Apply(probe);

  const auto after_fp = FingerprintBaseSql(connection, entry);
  Require(after_fp != before_fp,
          "Apply() did not change DuckDB Q" + std::to_string(entry.query_id) +
              " result — write did not invalidate observable query state");
  RequireSharedMatchesBase(connection, runtime, entry);

  auto restore = connection.Query(probe.restore_sql);
  Require(restore != nullptr && !restore->HasError(),
          restore ? restore->GetError() : "DuckDB probe restore failed");
}

void RunRfChunkDifferentialCheck(const std::string &db_path,
                                 const SharedQueryCatalogEntry &entry,
                                 int num_chunks, int order_count) {
  Require(entry.workload == aux_engine::SharedWorkloadKind::Tpch,
          "RF differential is TPC-H only");
  Require(entry.query_id != 11,
          "Q11 is unaffected by RF (no orders/lineitem dependency)");
  Require(num_chunks > 0, "num_chunks must be positive");
  Require(order_count > 0, "order_count must be positive");

  duckdb::DuckDB database(db_path.c_str());
  duckdb::Connection connection(database);
  DuckdbRuntime runtime(connection, entry, 10.0);
  runtime.Build();
  RequireSharedMatchesBase(connection, runtime, entry);

  const auto before_fp = FingerprintBaseSql(connection, entry);
  bool changed = false;
  int orders = order_count;
  int seq = 0;
  // Apply RF chunks (escalating size) until DuckDB's answer moves when possible.
  constexpr int kMaxAttempts = 8;
  for (int attempt = 0; attempt < kMaxAttempts && !changed; ++attempt) {
    runtime.ApplyRfChunk(seq++, orders);
    try {
      RequireSharedMatchesBase(connection, runtime, entry);
    } catch (const std::exception &ex) {
      throw std::runtime_error(std::string(ex.what()) + " (after RF chunk seq=" +
                               std::to_string(seq - 1) + " order_count=" +
                               std::to_string(orders) + ")");
    }
    if (FingerprintBaseSql(connection, entry) != before_fp) {
      changed = true;
      break;
    }
    orders = std::min(std::max(orders * 2, orders + 10), 200);
  }

  if (!changed) {
    // Standard TPC-H RF date ranges often miss historical predicates (e.g. Q3
    // TOP-10). Still require a write that invalidates this query: apply a
    // result-changing shared probe after RF maintenance has run.
    auto probe = FindInvalidatingSharedProbe(connection, entry);
    runtime.Apply(probe);
    const auto after_fp = FingerprintBaseSql(connection, entry);
    Require(after_fp != before_fp,
            "RF+probe did not change DuckDB Q" + std::to_string(entry.query_id) +
                " result — cannot confirm aux invalidation");
    RequireSharedMatchesBase(connection, runtime, entry);
    changed = true;
  }

  // Additional RF chunks (if requested) must stay correct after invalidation.
  while (seq < num_chunks) {
    runtime.ApplyRfChunk(seq++, order_count);
    try {
      RequireSharedMatchesBase(connection, runtime, entry);
    } catch (const std::exception &ex) {
      throw std::runtime_error(std::string(ex.what()) + " (after RF chunk seq=" +
                               std::to_string(seq - 1) + " order_count=" +
                               std::to_string(order_count) + ")");
    }
  }
  Require(changed, "internal: RF differential exited without invalidation");
}

} // namespace accelerator_codegen::aux_state::harness
