// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"
#include "accelerator_codegen/verus/verus_kit.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include <cctype>
#include <variant>
#include "duckdb/main/attached_database.hpp"
#include <map>
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <cmath>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "accelerator_codegen/runtime/duckdb/aux_query_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/harness/aux_state_footprint.hpp"
#include "accelerator_codegen/harness/cost_calibration.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace duckdb_backend = accelerator_codegen::aux_state::engine::duckdb;

// Scans normally run on the runtime's (writer's) connection. A reader thread
// (ReaderScope from another connection) must not touch it: a ScanRedirect
// sends its scans to a private connection, opened on first use.
struct ScanRedirect {
  ScanRedirect(::duckdb::DatabaseInstance &db_p, std::atomic<uint64_t> &opened_p)
      : db(db_p), opened(opened_p), previous(current) {
    current = this;
  }
  ~ScanRedirect() { current = previous; }
  ::duckdb::Connection &Connection() {
    if (!connection) {
      connection = std::make_unique<::duckdb::Connection>(db);
      opened++;
    }
    return *connection;
  }
  static thread_local ScanRedirect *current;

private:
  ::duckdb::DatabaseInstance &db;
  std::atomic<uint64_t> &opened;
  ScanRedirect *previous;
  std::unique_ptr<::duckdb::Connection> connection;
};
thread_local ScanRedirect *ScanRedirect::current = nullptr;

// SQL this thread ran against the tables (a read that runs any answered from
// SQL, not from the state).
thread_local uint64_t t_table_scans = 0;

} // namespace

// ---- engine services for the queries' InitialScan implementations ----
namespace scan {

void Require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void CivilFromDays(int32_t z, int32_t &year, unsigned &month, unsigned &day) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += (month <= 2);
}

std::string SqlDateLiteral(int32_t days) {
  int32_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, year, month, day);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "DATE '%04d-%02u-%02u'", year, month, day);
  return buf;
}

std::string SqlDateInPredicate(const char *column, const std::set<int32_t> &days) {
  if (days.empty())
    return "FALSE";
  const auto lo = *days.begin();
  const auto hi = *days.rbegin();
  const auto span = static_cast<int64_t>(hi) - lo + 1;
  if (days.size() > 64 || span <= static_cast<int64_t>(days.size()) * 4) {
    return std::string(column) + " BETWEEN " + SqlDateLiteral(lo) + " AND " +
           SqlDateLiteral(hi);
  }
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto day : days) {
    if (!first)
      sql << ", ";
    first = false;
    sql << SqlDateLiteral(day);
  }
  sql << ")";
  return sql.str();
}

std::string SqlIntInPredicate(const char *column, const std::set<int32_t> &values) {
  if (values.empty())
    return "FALSE";
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto value : values) {
    if (!first)
      sql << ", ";
    first = false;
    sql << value;
  }
  sql << ")";
  return sql.str();
}

std::string SqlInt64InPredicate(const char *column,
                                const std::unordered_set<int64_t> &values) {
  if (values.empty())
    return "FALSE";
  std::ostringstream sql;
  sql << column << " IN (";
  bool first = true;
  for (const auto value : values) {
    if (!first)
      sql << ", ";
    first = false;
    sql << value;
  }
  sql << ")";
  return sql.str();
}

// The scaled integer of a DECIMAL at its own scale (value = raw / 10^scale);
// the caller pairs it with that scale (core::ScaledDecimal).
int64_t DecimalRaw(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  const auto scale = value.type().id() == ::duckdb::LogicalTypeId::DECIMAL
                         ? static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()))
                         : 0;
  // GetValue<int64_t>() casts decimals like CAST(... AS BIGINT) and drops
  // fractional digits. Read the scaled integral payload instead.
  switch (value.type().InternalType()) {
  case ::duckdb::PhysicalType::INT16:
    return value.GetValueUnsafe<int16_t>();
  case ::duckdb::PhysicalType::INT32:
    return value.GetValueUnsafe<int32_t>();
  case ::duckdb::PhysicalType::INT64:
    return value.GetValueUnsafe<int64_t>();
  case ::duckdb::PhysicalType::INT128: {
    const auto huge = value.GetValueUnsafe<::duckdb::hugeint_t>();
    if (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) {
      return static_cast<int64_t>(huge.lower);
    }
    if (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0) {
      return static_cast<int64_t>(huge.lower);
    }
    break; // beyond 64 bits: through text (rounded)
  }
  default:
    break;
  }
  return static_cast<int64_t>(std::llround(std::stod(value.ToString()) * std::pow(10.0, scale)));
}

int64_t SafeInt64(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  switch (value.type().id()) {
  case ::duckdb::LogicalTypeId::BIGINT:
    return value.GetValue<int64_t>();
  case ::duckdb::LogicalTypeId::INTEGER:
    return value.GetValue<int32_t>();
  case ::duckdb::LogicalTypeId::SMALLINT:
    return value.GetValue<int16_t>();
  case ::duckdb::LogicalTypeId::TINYINT:
    return value.GetValue<int8_t>();
  case ::duckdb::LogicalTypeId::UINTEGER:
    return static_cast<int64_t>(value.GetValue<uint32_t>());
  case ::duckdb::LogicalTypeId::UBIGINT:
    return static_cast<int64_t>(value.GetValue<uint64_t>());
  case ::duckdb::LogicalTypeId::HUGEINT:
    return static_cast<int64_t>(value.GetValue<::duckdb::hugeint_t>().lower);
  default:
    return static_cast<int64_t>(std::llround(std::stod(value.ToString())));
  }
}

int32_t SafeInt32(const ::duckdb::Value &value) {
  return static_cast<int32_t>(SafeInt64(value));
}

int32_t DateDays(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return 0;
  }
  if (value.type().id() == ::duckdb::LogicalTypeId::DATE) {
    return ::duckdb::Date::EpochDays(value.GetValue<::duckdb::date_t>());
  }
  return SafeInt32(value);
}

std::string OptionalBenchScanLimitSuffix(const char *env_name) {
  const char *value = std::getenv(env_name);
  if (!value || !*value) {
    return "";
  }
  return std::string(" LIMIT ") + value;
}

::duckdb::Connection &ScanConnection(::duckdb::Connection &connection) {
  ++t_table_scans;
  return ScanRedirect::current ? ScanRedirect::current->Connection() : connection;
}

char FirstChar(const ::duckdb::Value &value) {
  if (value.IsNull()) {
    return '\0';
  }
  const auto text = value.ToString();
  return text.empty() ? '\0' : text[0];
}

} // namespace scan

// ---- the aux query registry (aux_query_registry.hpp) ----

namespace {

std::vector<std::unique_ptr<AuxQuery>> &AuxQueryStore() {
  static std::vector<std::unique_ptr<AuxQuery>> store;
  return store;
}

} // namespace

void RegisterAuxQuery(AuxQuery query) {
  AuxQueryStore().push_back(std::make_unique<AuxQuery>(std::move(query)));
}

std::vector<const AuxQuery *> AuxQueries() {
  std::vector<const AuxQuery *> out;
  for (const auto &query : AuxQueryStore()) {
    out.push_back(query.get());
  }
  return out;
}

const AuxQuery *FindAuxQuery(const std::string &accelerator_id) {
  for (const auto &query : AuxQueryStore()) {
    if (query->accelerator_id == accelerator_id) {
      return query.get();
    }
  }
  return nullptr;
}

namespace {

namespace core = accelerator_codegen::aux_state::core;
using namespace scan;

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

struct Builder final : core::StateBuilder {
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

// ---- Verus accelerators registered through the host kit (verus_kit.hpp) ----

core::AuxValue CapturedValue(const ::duckdb::Value &value);

// The kit's engine services on a runtime's connection: SQL as the
// connection's transaction sees it (a writer's own writes included).
struct DuckVerusHost final : verus_kit::Host {
  explicit DuckVerusHost(::duckdb::Connection &connection_p) : connection(connection_p) {}

  void Query(const std::string &sql, const verus_kit::RowFn &row) override {
    ScanQueryStreaming(connection, sql, [&](const ::duckdb::DataChunk &chunk, ::duckdb::idx_t r) {
      core::AuxRow out;
      out.values.reserve(chunk.ColumnCount());
      for (::duckdb::idx_t c = 0; c < chunk.ColumnCount(); ++c)
        out.values.push_back(CapturedValue(chunk.GetValue(c, r)));
      row(out);
    });
  }

  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params,
                   const verus_kit::RowFn &row) override {
    auto stmt = ScanConnection(connection).Prepare(sql);
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed: " + sql);
    ::duckdb::vector<::duckdb::Value> values;
    for (const auto &p : params) {
      if (p.is_null) {
        values.emplace_back();
        continue;
      }
      values.push_back(std::visit(
          [](const auto &payload) -> ::duckdb::Value {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, int64_t>)
              return ::duckdb::Value::BIGINT(payload);
            else if constexpr (std::is_same_v<T, uint64_t>)
              return ::duckdb::Value::UBIGINT(payload);
            else if constexpr (std::is_same_v<T, double>)
              return ::duckdb::Value::DOUBLE(payload);
            else if constexpr (std::is_same_v<T, bool>)
              return ::duckdb::Value::BOOLEAN(payload);
            else if constexpr (std::is_same_v<T, core::ScaledDecimal>)
              return ::duckdb::Value::DECIMAL(payload.value, 18, payload.scale);
            else if constexpr (std::is_same_v<T, core::AuxDate>) {
              ::duckdb::date_t d{};
              d.days = payload.days_since_epoch;
              return ::duckdb::Value::DATE(d);
            } else
              return ::duckdb::Value(payload);
          },
          p.payload));
    }
    auto result = stmt->Execute(values, /*allow_stream_result=*/false);
    Require(result && !result->HasError(), result ? result->GetError() : "query failed: " + sql);
    auto &m = dynamic_cast<::duckdb::MaterializedQueryResult &>(*result);
    for (::duckdb::idx_t r = 0; r < m.RowCount(); ++r) {
      core::AuxRow out;
      for (::duckdb::idx_t c = 0; c < m.ColumnCount(); ++c)
        out.values.push_back(CapturedValue(m.GetValue(c, r)));
      row(out);
    }
  }

  std::vector<int32_t> Attnums(const std::string &table, const std::vector<std::string> &columns) override {
    std::vector<std::pair<std::string, int32_t>> cids;
    auto result = ScanConnection(connection).Query("SELECT name, cid FROM pragma_table_info(" +
                                                   ::duckdb::KeywordHelper::WriteQuoted(table, '\'') + ")");
    Require(result && !result->HasError(), result ? result->GetError() : "table_info failed");
    for (::duckdb::idx_t r = 0; r < result->RowCount(); ++r)
      cids.emplace_back(result->GetValue(0, r).ToString(),
                        static_cast<int32_t>(result->GetValue(1, r).GetValue<int64_t>()));
    auto lower = [](std::string x) {
      for (auto &c : x)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return x;
    };
    std::vector<int32_t> out;
    for (const auto &col : columns) {
      int32_t found = -1;
      for (const auto &[name, cid] : cids)
        if (name == col)
          found = cid;
      if (found < 0)
        for (const auto &[name, cid] : cids)
          if (lower(name) == lower(col))
            found = cid;
      Require(found >= 0, "table " + table + " has no column " + col);
      out.push_back(found);
    }
    return out;
  }

  ::duckdb::Connection &connection;
};

struct KitBinding final : DuckdbRuntime::Binding {
  KitBinding(::duckdb::Connection &connection, const verus_kit::Glue &glue_p)
      : host(connection), glue(glue_p), module(verus_kit::MakeModule(glue_p, host)) {
    // Resolve the captured columns now: capture asks for them mid-statement.
    module->Dependencies();
  }
  core::AuxQueryModule &Module() override { return *module; }
  void Build(DuckDBAuxBackend &backend, core::StateBuilder &builder) override { module->Build(backend, builder); }
  void Execute(DuckDBAuxBackend &backend, const core::QueryArguments &arguments, core::AuxRowSink &sink) override {
    module->Execute(backend, arguments, sink);
  }
  std::vector<std::pair<std::string, core::TableId>> CaptureTables() const override {
    std::vector<std::pair<std::string, core::TableId>> out;
    for (std::size_t i = 0; i < glue.tables.size(); ++i)
      out.emplace_back(glue.tables[i].name, verus_kit::TableIdOf(i));
    return out;
  }
  DuckVerusHost host;
  const verus_kit::Glue &glue;
  std::unique_ptr<core::AuxQueryModule> module;
};

std::unique_ptr<DuckdbRuntime::Binding>
CreateBinding(::duckdb::Connection &connection,
              const SharedQueryCatalogEntry &entry) {
  Require(entry.accelerator_id != nullptr, "catalog entry without an accelerator id");
  if (const auto *glue = verus_kit::Find(entry.accelerator_id)) {
    return std::make_unique<KitBinding>(connection, *glue);
  }
  const auto *query = FindAuxQuery(entry.accelerator_id);
  Require(query != nullptr && static_cast<bool>(query->make_binding),
          std::string("no aux query registered for accelerator ") + entry.accelerator_id);
  return query->make_binding(connection);
}

struct ActiveRuntimeKey {
  ::duckdb::DatabaseInstance *db = nullptr;
  std::string accelerator_id;

  bool operator==(const ActiveRuntimeKey &other) const {
    return db == other.db && accelerator_id == other.accelerator_id;
  }
};

struct ActiveRuntimeKeyHash {
  std::size_t operator()(const ActiveRuntimeKey &key) const {
    return std::hash<::duckdb::DatabaseInstance *>()(key.db) ^
           (std::hash<std::string>()(key.accelerator_id) << 1);
  }
};

std::mutex &ActiveRuntimeMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<ActiveRuntimeKey, DuckdbRuntime *, ActiveRuntimeKeyHash> &
ActiveRuntimes() {
  static std::unordered_map<ActiveRuntimeKey, DuckdbRuntime *,
                            ActiveRuntimeKeyHash>
      runtimes;
  return runtimes;
}

void RegisterActiveRuntime(DuckdbRuntime *runtime,
                           ::duckdb::Connection &connection,
                           const SharedQueryCatalogEntry &entry) {
  if (!runtime || !entry.accelerator_id || !connection.context ||
      !connection.context->db) {
    return;
  }
  ActiveRuntimeKey key{connection.context->db.get(), entry.accelerator_id};
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  ActiveRuntimes()[key] = runtime;
}

void UnregisterActiveRuntime(DuckdbRuntime *runtime) {
  if (!runtime) {
    return;
  }
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  auto &runtimes = ActiveRuntimes();
  for (auto it = runtimes.begin(); it != runtimes.end();) {
    if (it->second == runtime) {
      it = runtimes.erase(it);
    } else {
      ++it;
    }
  }
}

DuckdbRuntime *FindActiveRuntimeImpl(::duckdb::DatabaseInstance &db,
                                     const std::string &accelerator_id) {
  ActiveRuntimeKey key{&db, accelerator_id};
  std::lock_guard<std::mutex> lock(ActiveRuntimeMutex());
  const auto it = ActiveRuntimes().find(key);
  return it == ActiveRuntimes().end() ? nullptr : it->second;
}

} // namespace

DuckdbRuntime *DuckdbRuntime::FindActive(::duckdb::DatabaseInstance &db,
                                         const std::string &accelerator_id) {
  return FindActiveRuntimeImpl(db, accelerator_id);
}

namespace {

// The oldest start time of an open transaction: aux versions older than the
// one it selects can be pruned.
DuckDBStateStore::FloorProvider OldestSnapshotFloor(::duckdb::ClientContext &context) {
  auto db = AuxCommitClock::DefaultDatabase(context);
  if (!db) {
    return {};
  }
  return [db]() -> core::Version {
    auto &manager = db->GetTransactionManager();
    if (!manager.IsDuckTransactionManager()) {
      return std::numeric_limits<core::Version>::max();
    }
    return manager.Cast<::duckdb::DuckTransactionManager>().LowestActiveStart();
  };
}

// The snapshot epoch from which a state built now is current: the start
// time of a transaction begun now (every commit so far is in it; no captured
// commit runs during the build, since commits go through this connection).
// GetLastCommit() is not on the start-time scale for a database opened from a
// file, so it cannot be used here.
core::Version BuildEpoch(::duckdb::Connection &connection) {
  if (connection.HasActiveTransaction()) {
    return DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  }
  connection.BeginTransaction();
  const auto epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  connection.Rollback();
  return epoch;
}

} // namespace

namespace {

// ---- Engine capture of writes ----
//
// The DuckDB fork's MutationRecorder reports each insert / update / delete a
// connection makes, with the columns the database's watch provider asks for,
// to every MutationConsumer registered on that connection.

// The watch provider: per table, the columns some runtime's module projects
// (union over runtimes, reference-counted). Registered in the database's
// object cache under the key MutationWatchProvider::TryGet looks up.
class AuxWatchedColumns final : public ::duckdb::MutationWatchProvider {
public:
  static constexpr const char *kKey = "accel_state_registry";
  static ::duckdb::string ObjectType() { return "MutationWatchProvider"; }

  void Add(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto c : columns) {
      watched[table][c]++;
    }
  }
  void Remove(const std::string &table, const std::vector<int32_t> &columns) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table);
    if (it == watched.end()) {
      return;
    }
    for (auto c : columns) {
      auto col = it->second.find(c);
      if (col != it->second.end() && --col->second == 0) {
        it->second.erase(col);
      }
    }
    if (it->second.empty()) {
      watched.erase(it);
    }
  }
  bool TryGetWatchedColumns(::duckdb::TableCatalogEntry &table,
                            ::duckdb::vector<::duckdb::PhysicalIndex> &columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    columns.clear();
    for (const auto &c : it->second) {
      columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return !columns.empty();
  }
  // Updates report the updated columns' new values plus the old values of
  // every watched column, so a capture can rebuild the old and new rows.
  bool TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                              const ::duckdb::vector<::duckdb::PhysicalIndex> &updated,
                              ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
      return false;
    }
    bool relevant = false;
    for (const auto &u : updated) {
      relevant = relevant || it->second.count(static_cast<int32_t>(u.index)) > 0;
    }
    if (!relevant) {
      return false;
    }
    old_columns.clear();
    for (const auto &c : it->second) {
      old_columns.emplace_back(static_cast<::duckdb::idx_t>(c.first));
    }
    return true;
  }

private:
  std::mutex mutex;
  std::unordered_map<std::string, std::map<int32_t, int>> watched;
};

AuxWatchedColumns &WatchedColumns(::duckdb::ClientContext &context) {
  auto &cache = ::duckdb::DatabaseInstance::GetDatabase(context).GetObjectCache();
  auto entry = cache.GetOrCreate<AuxWatchedColumns>(AuxWatchedColumns::kKey);
  return *entry;
}

// Forwards the connection's mutation events to one runtime.
class AuxCaptureConsumer final : public ::duckdb::MutationConsumer {
public:
  explicit AuxCaptureConsumer(DuckdbRuntime *runtime_p) : runtime(runtime_p) {}
  void OnTableMutation(::duckdb::ClientContext &,
                       const ::duckdb::TableMutationEvent &event) override {
    runtime->OnCapturedMutation(event);
  }

private:
  DuckdbRuntime *runtime;
};

// A captured DuckDB value as the modules read it (the same mapping as the
// PostgreSQL capture: integers Int64, dates AuxDate, decimals ScaledDecimal,
// doubles Double, everything else its text).
core::AuxValue CapturedValue(const ::duckdb::Value &value) {
  using ::duckdb::LogicalTypeId;
  const auto id = value.type().id();
  switch (id) {
  case LogicalTypeId::BOOLEAN:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Boolean)
                          : core::AuxValue::Of(core::AuxType::Boolean, value.GetValue<bool>());
  case LogicalTypeId::TINYINT:
  case LogicalTypeId::SMALLINT:
  case LogicalTypeId::INTEGER:
  case LogicalTypeId::BIGINT:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Int64)
                          : core::AuxValue::Of(core::AuxType::Int64, value.GetValue<int64_t>());
  case LogicalTypeId::DATE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Date)
                          : core::AuxValue::Of(core::AuxType::Date, core::AuxDate {DateDays(value)});
  case LogicalTypeId::FLOAT:
  case LogicalTypeId::DOUBLE:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::Double)
                          : core::AuxValue::Of(core::AuxType::Double, value.GetValue<double>());
  case LogicalTypeId::DECIMAL: {
    if (value.IsNull()) {
      return core::AuxValue::Null(core::AuxType::ScaledDecimal);
    }
    core::ScaledDecimal decimal;
    decimal.value = DecimalRaw(value);
    decimal.scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()));
    return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
  }
  default:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, value.ToString());
  }
}

} // namespace

void DuckdbRuntime::EnableMutationCapture() {
  if (capture_enabled_) {
    return;
  }
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Add(table.first, dep.mutation_attnums);
      }
    }
  }
  connection_.context->registered_state->GetOrCreate<AuxCaptureConsumer>(
      std::string("accel_aux_capture:") + entry_.accelerator_id, this);
  capture_enabled_ = true;
}

void DuckdbRuntime::DisableMutationCapture() {
  if (!capture_enabled_) {
    return;
  }
  connection_.context->registered_state->Remove(std::string("accel_aux_capture:") +
                                                entry_.accelerator_id);
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Remove(table.first, dep.mutation_attnums);
      }
    }
  }
  capture_enabled_ = false;
}

void DuckdbRuntime::OnCapturedMutation(const ::duckdb::TableMutationEvent &event) {
  core::TableId table_id = 0;
  bool known = false;
  for (const auto &table : binding_->CaptureTables()) {
    if (table.first == event.table_name) {
      table_id = table.second;
      known = true;
    }
  }
  if (!known) {
    return;
  }
  for (const auto &dep : binding_->Module().Dependencies()) {
    if (dep.table != table_id || dep.mutation_attnums.empty()) {
      continue;
    }
    core::MutationBatch batch;
    batch.table = table_id;
    batch.kind = event.kind == ::duckdb::TableMutationKind::INSERT   ? core::MutationKind::Insert
                 : event.kind == ::duckdb::TableMutationKind::DELETE ? core::MutationKind::Delete
                                                                     : core::MutationKind::Update;
    if (batch.kind == core::MutationKind::Update) {
      bool relevant = false;
      for (std::size_t j = 0; j < dep.mutation_attnums.size(); ++j) {
        for (const auto &u : event.updated_columns) {
          if (static_cast<int32_t>(u.index) == dep.mutation_attnums[j]) {
            batch.updated_columns.push_back(static_cast<core::ColumnId>(j));
            relevant = true;
          }
        }
      }
      if (!relevant) {
        continue;
      }
    }
    for (const auto &row : event.rows) {
      // Old values first (watched columns), then the updated columns' new
      // values: the new row is the old one with those replaced.
      std::unordered_map<int32_t, const ::duckdb::Value *> old_values, new_values;
      const std::size_t updated = event.updated_columns.size();
      const std::size_t olds = row.values.size() - (batch.kind == core::MutationKind::Update ? updated : 0);
      for (std::size_t i = 0; i < row.values.size(); ++i) {
        const auto column = static_cast<int32_t>(row.values[i].column_index.index);
        if (batch.kind == core::MutationKind::Update && i >= olds) {
          new_values[column] = &row.values[i].value;
        } else {
          old_values[column] = &row.values[i].value;
        }
      }
      auto project = [&](bool use_new) {
        core::AuxRow out;
        out.values.reserve(dep.mutation_attnums.size());
        for (auto attnum : dep.mutation_attnums) {
          const ::duckdb::Value *v = nullptr;
          if (use_new) {
            auto it = new_values.find(attnum);
            if (it != new_values.end()) {
              v = it->second;
            }
          }
          if (!v) {
            auto it = old_values.find(attnum);
            if (it != old_values.end()) {
              v = it->second;
            }
          }
          out.values.push_back(v ? CapturedValue(*v) : core::AuxValue::Null(core::AuxType::String));
        }
        return out;
      };
      switch (batch.kind) {
      case core::MutationKind::Insert:
        batch.new_rows.push_back(project(false));
        break;
      case core::MutationKind::Delete:
        batch.old_rows.push_back(project(false));
        break;
      case core::MutationKind::Update:
        batch.old_rows.push_back(project(false));
        batch.new_rows.push_back(project(true));
        break;
      }
    }
    if (!batch.old_rows.empty() || !batch.new_rows.empty()) {
      backend_.CaptureMutation(batch);
    }
  }
}

void DuckdbRuntime::BeginWrite() { connection_.BeginTransaction(); }

void DuckdbRuntime::ApplyWrite(const std::string &sql) {
  BeginWrite();
  auto result = connection_.Query(sql);
  if (!result || result->HasError()) {
    RollbackWrite();
    throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
  }
  CommitWrite();
}

std::vector<core::CalibrationWrite> DuckdbRuntime::CalibrationWrites() const {
  return binding_->Module().CalibrationWrites();
}

core::CalibrationEngine::Timed
DuckdbCalibrationEngine::WriteThenRead(const std::string &sql,
                                       std::optional<maintenance::MaintenanceAction> forced) {
  core::SetForcedMaintenanceActionName(forced ? maintenance::MaintenanceActionName(*forced) : "");
  core::ClearMaintenanceDecisionLog();
  Timed timed;
  const auto start = std::chrono::steady_clock::now();
  try {
    runtime.ApplyWrite(sql);
    const auto read_start = std::chrono::steady_clock::now();
    runtime.Execute();
    timed.read_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                              read_start)
                        .count();
  } catch (...) {
    core::SetForcedMaintenanceActionName("");
    throw;
  }
  timed.us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  core::SetForcedMaintenanceActionName("");
  timed.decision = core::LastMaintenanceDecision();
  return timed;
}

void DuckdbRuntime::RollbackWrite() {
  connection_.Rollback();
  coordinator_.Abort();
}

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection,
                             const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry),
      clock_(connection.context->registered_state->GetOrCreate<AuxCommitClock>(
          "accel_aux_commit_clock", AuxCommitClock::DefaultDatabase(*connection.context))),
      state_store_(entry.accelerator_id,
                   [clock = clock_] { return clock->epoch.load(); },
                   OldestSnapshotFloor(*connection.context)),
      // Same epochs and floor as state_store_: the two stores differ only
      // in their protocol's implementation.
      verus_store_(VerusAuxStoreRequested()
                       ? MakeVerusAuxStore(entry.accelerator_id,
                                           [clock = clock_] { return clock->epoch.load(); },
                                           OldestSnapshotFloor(*connection.context))
                       : nullptr),
      coordinator_(),
      expected_queries_before_write_(
          std::make_shared<double>(expected_queries_before_write)),
      backend_(coordinator_, ActiveStore(),
               [](const ::duckdb::TableMutationEvent &) {
                 return core::MutationBatch{};
               },
               [] { return DuckDBAuxBackend::ReaderVisibility(); },
               [entry] {
                 return core::LoadAuxCostModelJson(
                     harness::SharedCostModelConfigPath(entry.workload,
                                                        entry.query_id));
               },
               [expected = expected_queries_before_write_]() {
                 return *expected;
               }),
      binding_(CreateBinding(connection, entry)) {
  coordinator_.Register(std::shared_ptr<core::AuxQueryModule>(
      &binding_->Module(), [](core::AuxQueryModule *) {}));
  backend_.SetExecutionContext(connection.context.get());
}

void DuckdbRuntime::CommitWrite() {
  // Readers whose transaction starts after this writer's may see the
  // commit: they wait for its publish. (The commit hook then moves the
  // epoch to the commit id + 1 for the publish.)
  clock_->epoch = DuckDBAuxBackend::SnapshotEpoch(*connection_.context) + 1;
  try {
    coordinator_.PreCommit();
    connection_.Commit();
  } catch (...) {
    coordinator_.Abort();
    throw;
  }
  coordinator_.Commit();
}

void DuckdbRuntime::SetExpectedQueriesBeforeNextWrite(
    double expected_queries_before_write) {
  *expected_queries_before_write_ = expected_queries_before_write;
}

double DuckdbRuntime::ExpectedQueriesBeforeNextWrite() const {
  return *expected_queries_before_write_;
}

void DuckdbRuntime::Build(bool register_active) {
  const auto build_epoch = BuildEpoch(connection_);
  Builder builder;
  binding_->Build(backend_, builder);
  Require(builder.finished, "shared DuckDB build did not finish");
  // Current from the snapshot the build read (no captured commit can run
  // meanwhile: commits go through this runtime's connection).
  if (verus_store_) {
    verus_store_->Install(std::move(builder.images), entry_.accelerator_id,
                          build_epoch);
  } else {
    state_store_.Install(std::move(builder.images), entry_.accelerator_id,
                         build_epoch);
  }
  if (register_active) {
    RegisterActiveRuntime(this, connection_, entry_);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  AuxReadScope one_version; // every read in this query: one state version
  // Normally unused: commits publish valid state (ReadersCanBuildState).
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks);
  }
  MaterializeSink sink;
  t_table_scans = 0;
  binding_->Execute(backend_, arguments, sink);
  core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                           : core::AuxExecutePath::Hit);
  last_rows = sink.rows.size();
  return std::move(sink.rows);
}

void DuckdbRuntime::Execute() {
  Execute(ToCoreArguments(entry_.benchmark_arguments));
}

void DuckdbRuntime::Execute(const core::QueryArguments &arguments) {
  core::BeginAuxExecuteProfiling();
  auto rows = ExecuteCollect(arguments);
  core::EndAuxExecuteProfiling();
  last_rows = rows.size();
}

void DuckdbRuntime::Apply(SharedProbe &probe) {
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  connection_.BeginTransaction();
  auto result = connection_.Query(ProbeWriteSql(probe));
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB write failed");
  backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
  CommitWrite();
  AdvanceProbe(probe);
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count()));
}

void DuckdbRuntime::ApplyBatch(std::vector<SharedProbe> &probes) {
  Require(!probes.empty(), "ApplyBatch requires at least one probe");
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  connection_.BeginTransaction();
  for (auto &probe : probes) {
    auto result = connection_.Query(ProbeWriteSql(probe));
    Require(result != nullptr && !result->HasError(),
            result ? result->GetError() : "DuckDB batch write failed");
    backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
    AdvanceProbe(probe);
  }
  CommitWrite();
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count()));
}

void DuckdbRuntime::ApplyRfChunk(int write_seq, int order_count) {
  if (entry_.workload != SharedWorkloadKind::Tpch) {
    throw std::runtime_error("ApplyRfChunk is only supported for TPC-H");
  }
  // Q11 does not watch orders/lineitem; RF would mutate the DB without
  // invalidating aux. Skip capture (and SQL) so RF sweeps stay meaningful.
  if (entry_.query_id == 11) {
    return;
  }
  connection_.BeginTransaction();
  const auto plan =
      NextTpchRfChunkPlan(write_seq, order_count, connection_);
  const auto applied =
      ApplyTpchRfChunkSql(connection_, entry_.query_id, plan);
  for (const auto &mutation : applied.mutations) {
    if (!mutation.old_rows.empty() || !mutation.new_rows.empty()) {
      backend_.CaptureMutation(mutation);
    }
  }
  CommitWrite();
}

void DuckdbRuntime::ResetCalibrationState() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
  coordinator_.Abort();
}

void DuckdbRuntime::InvalidateAll() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
}

core::AuxStateStore &DuckdbRuntime::ActiveStore() {
  if (verus_store_) {
    return *verus_store_;
  }
  return state_store_;
}

std::size_t DuckdbRuntime::StateVersionCount() const {
  return verus_store_ ? verus_store_->TotalVersionCount()
                      : state_store_.TotalVersionCount();
}

std::size_t DuckdbRuntime::StaleInstallCount() const {
  return verus_store_ ? verus_store_->StaleInstalls() : 0;
}

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  // No engine transaction: publish for every snapshot (epoch 0).
  clock_->epoch = 0;
  backend_.CaptureMutation(mutation);
  coordinator_.PreCommit();
  coordinator_.Commit();
}

void DuckdbRuntime::ExecuteBaseQuery() {
  auto result = connection_.Query(entry_.base_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB base query failed");
  for (::duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (::duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
}

core::AuxStoreDiagnosticsSnapshot DuckdbRuntime::StoreDiagnosticsSnapshot() const {
  if (verus_store_) {
    return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
        static_cast<uint64_t>(verus_store_->ValidPartitionCount()),
        static_cast<uint64_t>(verus_store_->TotalPartitionCount()));
  }
  return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
      static_cast<uint64_t>(state_store_.ValidPartitionCount()),
      static_cast<uint64_t>(state_store_.TotalPartitionCount()));
}

void DuckdbRuntime::ResetStoreDiagnosticsProfiling() {
  core::ResetAuxStoreDiagnosticsProfiling();
}

uint64_t DuckdbRuntime::AuxStateFootprintBytes() const {
  if (verus_store_) {
    return 0; // the Verus store's states are not measured
  }
  return harness::ComputeAuxStateFootprintBytes(state_store_,
                                                entry_.accelerator_id);
}

DuckdbRuntime::~DuckdbRuntime() {
  UnregisterActiveRuntime(this);
  DisableMutationCapture();
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
