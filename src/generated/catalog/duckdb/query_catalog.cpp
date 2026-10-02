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

const SharedQueryCatalogEntry *VerusKitCatalogEntry(const std::string &accelerator_id) {
  const auto *glue = verus_kit::Find(accelerator_id);
  if (!glue) {
    return nullptr;
  }
  const auto *base = FindEntry(ParseWorkload(glue->workload), glue->query_id);
  if (!base) {
    return nullptr;
  }
  return &Derived(accelerator_id, *base, std::string("SELECT * FROM accel_") + glue->id + "()", false);
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
