// GENERATED BACKEND
// The shared query catalog: entries are registered by each query's directory
// (src/generated/<bench>/q<N>/duckdb/aux/register.cpp, SharedQueryRegistrar);
// this file names no query.
#include "catalog/duckdb/query_catalog.hpp"

#include "accelerator_codegen/verus/verus_kit.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <stdexcept>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine {
namespace {

namespace core = accelerator_codegen::aux_state::core;
using catalog_values::Require;

SharedWorkloadKind ParseWorkload(const std::string &workload_name) {
  if (workload_name == "tpch") {
    return SharedWorkloadKind::Tpch;
  }
  if (workload_name == "sec_edgar") {
    return SharedWorkloadKind::SecEdgar;
  }
  throw std::runtime_error("unsupported workload: " + workload_name);
}

// Registered entries (stable addresses), and variants: other accelerators
// serving a registered query.
struct Variant {
  std::string accelerator_id;
  SharedWorkloadKind workload;
  int query_id;
  std::string duckdb_aux_sql;
};

struct Registry {
  std::mutex mutex;
  std::deque<SharedQueryCatalogEntry> entries;
  std::vector<Variant> variants;
  // derived entries (variants, `_aux_parallel`, Verus kit glue), by accelerator id
  std::map<std::string, std::unique_ptr<std::pair<std::string, SharedQueryCatalogEntry>>> derived;
};

Registry &TheRegistry() {
  static Registry registry;
  return registry;
}

const SharedQueryCatalogEntry *FindEntry(SharedWorkloadKind workload, int query_id) {
  for (const auto &entry : TheRegistry().entries) {
    if (entry.workload == workload && entry.query_id == query_id) {
      return &entry;
    }
  }
  return nullptr;
}

// `base` under another accelerator id and aux SQL (cached by id).
const SharedQueryCatalogEntry &Derived(const std::string &accelerator_id, const SharedQueryCatalogEntry &base,
                                       const std::string &aux_sql, bool postgres_aux_too) {
  auto &registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto &slot = registry.derived[accelerator_id];
  if (!slot) {
    slot = std::make_unique<std::pair<std::string, SharedQueryCatalogEntry>>(aux_sql, base);
    // the id lives in the map key; the SQL in the pair (stable addresses)
    auto it = registry.derived.find(accelerator_id);
    slot->second.accelerator_id = it->first.c_str();
    slot->second.duckdb_aux_sql = slot->first.c_str();
    if (postgres_aux_too) {
      slot->second.postgres_aux_sql = slot->first.c_str();
    }
  }
  return slot->second;
}

} // namespace

void RegisterSharedQuery(SharedQueryCatalogEntry entry) {
  auto &registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.entries.push_back(std::move(entry));
}

void RegisterSharedQueryVariant(std::string accelerator_id, SharedWorkloadKind workload, int query_id,
                                std::string duckdb_aux_sql) {
  auto &registry = TheRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.variants.push_back({std::move(accelerator_id), workload, query_id, std::move(duckdb_aux_sql)});
}

const SharedQueryCatalogEntry *FindSharedQueryByAccelerator(const std::string &accelerator_id) {
  for (const auto &entry : TheRegistry().entries) {
    if (entry.accelerator_id && accelerator_id == entry.accelerator_id) {
      return &entry;
    }
  }
  for (const auto &variant : TheRegistry().variants) {
    if (variant.accelerator_id == accelerator_id) {
      const auto *base = FindEntry(variant.workload, variant.query_id);
      return base ? &Derived(accelerator_id, *base, variant.duckdb_aux_sql, false) : nullptr;
    }
  }
  if (IsParallelAuxAccelerator(accelerator_id.c_str())) {
    const auto base_id = accelerator_id.substr(0, accelerator_id.size() - 13);
    for (const auto &entry : TheRegistry().entries) {
      if (entry.accelerator_id && base_id == entry.accelerator_id) {
        return &LookupSharedQueryParallel(entry.workload, entry.query_id);
      }
    }
  }
  return VerusKitCatalogEntry(accelerator_id);
}

core::QueryArguments ToCoreArguments(const SharedQueryArguments &arguments) {
  return core::QueryArguments{arguments.values};
}

const SharedQueryCatalogEntry &LookupSharedQuery(SharedWorkloadKind workload,
                                               int query_id) {
  const auto *entry = FindEntry(workload, query_id);
  Require(entry != nullptr, "unsupported shared query (none registered): " + std::to_string(query_id));
  return *entry;
}

static core::AuxType KitArgType(verus_kit::Kind kind) {
  switch (kind) {
  case verus_kit::Kind::Int:
    return core::AuxType::Int64;
  case verus_kit::Kind::Dec:
    return core::AuxType::ScaledDecimal; // exact: a double turns 0.06 + 0.01 into 0.0699...
  case verus_kit::Kind::Date:
    return core::AuxType::Date;
  case verus_kit::Kind::Bool:
    return core::AuxType::Boolean;
  case verus_kit::Kind::Text:
    break;
  }
  return core::AuxType::String;
}

const SharedQueryCatalogEntry *VerusKitCatalogEntry(const std::string &accelerator_id) {
  const auto *glue = verus_kit::Find(accelerator_id);
  if (!glue) {
    return nullptr;
  }
  const auto workload = ParseWorkload(glue->workload);
  // Derive from another accelerator's entry for the query only when its
  // arguments are the glue's: the table function types each argument by the
  // entry (a C++ accelerator that derives a parameter itself takes fewer).
  if (const auto *base = FindEntry(workload, glue->query_id)) {
    bool same = base->benchmark_arguments.values.size() == glue->params.size();
    for (std::size_t i = 0; same && i < glue->params.size(); ++i)
      same = base->benchmark_arguments.values[i].type == KitArgType(glue->params[i]);
    if (same)
      return &Derived(accelerator_id, *base, std::string("SELECT * FROM accel_") + glue->id + "()", false);
  }
  // No other accelerator registered the query: the entry from the glue alone
  // (its parameterized SQL; arguments typed by its parameter kinds; no probes).
  struct Own {
    std::string id, workload_name, sql, aux_sql;
    SharedQueryCatalogEntry entry;
  };
  static std::mutex mutex;
  static std::map<std::string, std::unique_ptr<Own>> own;
  std::lock_guard<std::mutex> lock(mutex);
  auto &slot = own[accelerator_id];
  if (!slot) {
    slot = std::make_unique<Own>();
    slot->id = glue->id;
    slot->workload_name = glue->workload;
    slot->sql = glue->query_sql ? glue->query_sql : "";
    slot->aux_sql = std::string("SELECT * FROM accel_") + glue->id + "()";
    auto &e = slot->entry;
    e.query_id = glue->query_id;
    e.workload = workload;
    e.workload_name = slot->workload_name.c_str();
    e.accelerator_id = slot->id.c_str();
    e.base_sql = slot->sql.c_str();
    e.duckdb_aux_sql = slot->aux_sql.c_str();
    for (const auto kind : glue->params)
      e.benchmark_arguments.values.push_back(core::AuxValue::Null(KitArgType(kind)));
  }
  return &slot->entry;
}

const SharedQueryCatalogEntry &LookupSharedQuery(const std::string &workload_name,
                                               int query_id) {
  return LookupSharedQuery(ParseWorkload(workload_name), query_id);
}

bool IsParallelAuxAccelerator(const char *accelerator_id) {
  if (!accelerator_id) {
    return false;
  }
  const std::string id(accelerator_id);
  return id.size() >= 13 && id.compare(id.size() - 13, 13, "_aux_parallel") == 0;
}

const SharedQueryCatalogEntry &LookupSharedQueryParallel(SharedWorkloadKind workload,
                                                       int query_id) {
  Require(workload == SharedWorkloadKind::SecEdgar,
          "parallel aux catalog is only implemented for sec_edgar");
  const auto &base = LookupSharedQuery(workload, query_id);
  const std::string id = std::string(base.accelerator_id) + "_aux_parallel";
  return Derived(id, base, "SELECT * FROM accel_" + id + "()", true);
}

const SharedQueryCatalogEntry &LookupSharedQueryParallel(const std::string &workload_name,
                                                       int query_id) {
  return LookupSharedQueryParallel(ParseWorkload(workload_name), query_id);
}

std::vector<int> SharedQueriesForWorkload(SharedWorkloadKind workload) {
  std::vector<int> out;
  for (const auto &entry : TheRegistry().entries) {
    if (entry.workload == workload) {
      out.push_back(entry.query_id);
    }
  }
  std::sort(out.begin(), out.end());
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
  if (probe.toggles && probe.flipped)
    std::swap(mutation.old_rows, mutation.new_rows);
  (void)entry;
  return mutation;
}

RowidProbeTarget ParseRowidProbe(const SharedProbe &probe) {
  // "UPDATE <table> SET <column> = <column> + <delta> WHERE rowid = <id>"
  const auto &sql = probe.update_sql;
  const auto fail = [&]() {
    throw std::runtime_error("not an additive rowid probe: " + sql);
  };
  if (probe.toggles || probe.repeats || sql.rfind("UPDATE ", 0) != 0)
    fail();
  const auto set_pos = sql.find(" SET ");
  const auto eq_pos = sql.find(" = ", set_pos == std::string::npos ? 0 : set_pos);
  const auto plus_pos = sql.find(" + ", eq_pos == std::string::npos ? 0 : eq_pos);
  const auto where_pos = sql.rfind(" WHERE rowid = ");
  if (set_pos == std::string::npos || eq_pos == std::string::npos ||
      plus_pos == std::string::npos || where_pos == std::string::npos ||
      !(set_pos < eq_pos && eq_pos < plus_pos && plus_pos < where_pos))
    fail();
  RowidProbeTarget target;
  target.table = sql.substr(7, set_pos - 7);
  target.column = sql.substr(set_pos + 5, eq_pos - set_pos - 5);
  if (sql.substr(eq_pos + 3, plus_pos - eq_pos - 3) != target.column)
    fail();
  std::size_t parsed = 0;
  const auto id_text = sql.substr(where_pos + 15);
  target.rowid = std::stoll(id_text, &parsed);
  if (parsed != id_text.size())
    fail();
  return target;
}

BatchWritePool CollectBatchWritePool(::duckdb::Connection &connection,
                                     const SharedQueryCatalogEntry &entry,
                                     std::size_t want, std::size_t min_rows,
                                     const char *filler_select_sql) {
  Require(static_cast<bool>(entry.build_probe),
          "missing build_probe for Q" + std::to_string(entry.query_id));
  BatchWritePool pool;
  std::unordered_set<int64_t> seen;
  const auto take = [&](const std::string &sql, bool invalidating) {
    auto result = connection.Query(sql);
    Require(result != nullptr && !result->HasError(),
            result ? ("batch pool select failed for Q" + std::to_string(entry.query_id) +
                      ": " + result->GetError())
                   : "batch pool select returned null");
    auto &rows = result->Cast<::duckdb::MaterializedQueryResult>();
    for (::duckdb::idx_t row = 0; row < rows.RowCount() && pool.probes.size() < want; ++row) {
      auto probe = entry.build_probe(rows, row);
      const auto target = ParseRowidProbe(probe);
      if (pool.table.empty()) {
        pool.table = target.table;
        pool.column = target.column;
      }
      Require(target.table == pool.table && target.column == pool.column,
              "batch pool rows must update one table/column");
      if (!seen.insert(target.rowid).second)
        continue; // a row repeated by a join: count it once
      pool.rowids.push_back(target.rowid);
      pool.probes.push_back(std::move(probe));
      if (invalidating)
        ++pool.invalidating;
    }
  };
  // Over-fetch: joins (e.g. num x pre) repeat rows.
  const int limit = static_cast<int>(std::min<std::size_t>(want * 2 + 16, 1u << 30));
  take(ExpandProbeSelectLimit(entry.probe_select_sql, limit), true);
  if (pool.probes.size() < want && filler_select_sql != nullptr) {
    const int filler_limit = static_cast<int>(
        std::min<std::size_t>(want + pool.probes.size() + 16, 1u << 30));
    take(ExpandProbeSelectLimit(filler_select_sql, filler_limit), false);
  }
  Require(pool.probes.size() >= min_rows,
          "Q" + std::to_string(entry.query_id) + ": only " +
              std::to_string(pool.probes.size()) + " distinct rows for a batch of " +
              std::to_string(min_rows));
  return pool;
}

core::MutationBatch BuildSharedBatchMutation(
    const SharedQueryCatalogEntry &entry,
    const std::vector<const SharedProbe *> &probes) {
  Require(!probes.empty(), "empty batch mutation");
  core::MutationBatch mutation;
  mutation.table = probes.front()->table;
  mutation.kind = probes.front()->kind;
  mutation.updated_columns = probes.front()->updated_columns;
  mutation.old_rows.reserve(probes.size());
  mutation.new_rows.reserve(probes.size());
  for (const auto *probe : probes) {
    Require(probe->table == mutation.table && probe->kind == mutation.kind &&
                probe->updated_columns == mutation.updated_columns,
            "batch mutation rows must share table, kind and columns");
    auto one = BuildSharedMutation(entry, *probe);
    mutation.old_rows.push_back(std::move(one.old_rows.front()));
    mutation.new_rows.push_back(std::move(one.new_rows.front()));
  }
  return mutation;
}

const std::string &ProbeWriteSql(const SharedProbe &probe) {
  return probe.toggles && probe.flipped ? probe.restore_sql : probe.update_sql;
}

namespace {
// `next` = `now` + (`now` - `before`), for the numeric value kinds probes add to.
bool StepValue(const core::AuxValue &before, const core::AuxValue &now,
               core::AuxValue &next) {
  if (before.is_null || now.is_null)
    return false;
  if (const auto *b = std::get_if<core::ScaledDecimal>(&before.payload)) {
    const auto *n = std::get_if<core::ScaledDecimal>(&now.payload);
    if (!n || n->scale != b->scale)
      return false;
    next = core::AuxValue::Of(core::AuxType::ScaledDecimal,
                              core::ScaledDecimal{2 * n->value - b->value, n->scale});
    return true;
  }
  if (const auto *b = std::get_if<int64_t>(&before.payload)) {
    const auto *n = std::get_if<int64_t>(&now.payload);
    if (!n)
      return false;
    next = core::AuxValue::Of(core::AuxType::Int64, 2 * *n - *b);
    return true;
  }
  if (const auto *b = std::get_if<double>(&before.payload)) {
    const auto *n = std::get_if<double>(&now.payload);
    if (!n)
      return false;
    next = core::AuxValue::Of(core::AuxType::Double, 2 * *n - *b);
    return true;
  }
  return false;
}
} // namespace

void AdvanceProbe(SharedProbe &probe) {
  probe.current_value += probe.write_delta;
  if (probe.repeats)
    return;
  if (probe.toggles) {
    probe.flipped = !probe.flipped;
    return;
  }
  // An additive write (x = x + c): the next write starts from this one's new
  // row and adds the same step, so its reported rows match the table.
  // Every numeric column steps by its own difference (unchanged ones stay);
  // any other column must be unchanged.
  if (probe.old_row.values.size() != probe.new_row.values.size())
    return;
  auto next = probe.new_row;
  for (std::size_t c = 0; c < probe.new_row.values.size(); ++c) {
    const auto &before = probe.old_row.values[c];
    const auto &now = probe.new_row.values[c];
    if (StepValue(before, now, next.values[c]))
      continue;
    const auto *bs = std::get_if<std::string>(&before.payload);
    const auto *ns = std::get_if<std::string>(&now.payload);
    const auto *bd = std::get_if<core::AuxDate>(&before.payload);
    const auto *nd = std::get_if<core::AuxDate>(&now.payload);
    const bool unchanged = (before.is_null && now.is_null) ||
                           (bs && ns && *bs == *ns) ||
                           (bd && nd && bd->days_since_epoch == nd->days_since_epoch);
    if (!unchanged)
      return; // not an additive write: keep the rows as they are
  }
  probe.old_row = probe.new_row;
  probe.new_row = std::move(next);
}

} // namespace accelerator_codegen::aux_state::engine
