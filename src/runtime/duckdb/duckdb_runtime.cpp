// GENERATED BACKEND
#include "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"
#include "accelerator_codegen/runtime/duckdb/tpch_rf.hpp"
#include "accelerator_codegen/verus/verus_kit.hpp"
#include "accelerator_codegen/runtime/catalog/shared_query_engine.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include <chrono>
#include <atomic>
#include <cctype>
#include <variant>
#include "duckdb/main/attached_database.hpp"
#include <map>
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/accelerator/snapshot_override.hpp"
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
#include <algorithm>
#include <mutex>
#include <atomic>
#include <exception>
#include <thread>
#include <vector>
#include <shared_mutex>
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
// sends its scans to a private connection, opened on first use. That
// connection reads at the reader's snapshot (`snapshot`, the reader's
// transaction start time; 0 = the newest data), not at the newest commit:
// the SQL fallback must answer from the same state as the rest of the
// reader's transaction (snapshot isolation).
struct ScanRedirect {
  ScanRedirect(::duckdb::DatabaseInstance &db_p, std::atomic<uint64_t> &opened_p,
               ::duckdb::transaction_t snapshot_p = 0)
      : db(db_p), opened(opened_p), snapshot(snapshot_p), previous(current) {
    current = this;
  }
  ~ScanRedirect() {
    current = previous;
    if (connection && snapshot)
      ::duckdb::ClearTransactionStartOverride(*connection->context);
  }
  ::duckdb::Connection &Connection() {
    if (!connection) {
      connection = std::make_unique<::duckdb::Connection>(db);
      if (snapshot)
        ::duckdb::SetTransactionStartOverride(*connection->context, snapshot);
      opened++;
    }
    return *connection;
  }
  static thread_local ScanRedirect *current;

private:
  ::duckdb::DatabaseInstance &db;
  std::atomic<uint64_t> &opened;
  ::duckdb::transaction_t snapshot;
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

ScanLock LockScan(::duckdb::Connection &connection) {
  if (ScanRedirect::current)
    return ScanLock();
  static std::mutex map_mutex;
  static std::unordered_map<const ::duckdb::Connection *, std::unique_ptr<std::recursive_mutex>> locks;
  std::recursive_mutex *m;
  {
    std::lock_guard<std::mutex> g(map_mutex);
    auto &slot = locks[&connection];
    if (!slot)
      slot = std::make_unique<std::recursive_mutex>();
    m = slot.get();
  }
  return ScanLock(*m);
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

// One column of a DataChunk, read in place (see DuckVerusHost::Query).
struct ChunkColumn {
  enum class Kind { Bool, Int8, Int16, Int32, Int64, Date, Float, Double, Dec16, Dec32, Dec64, Dec128, Varchar, Other };
  Kind kind = Kind::Other;
  int32_t scale = 0;
  ::duckdb::UnifiedVectorFormat uvf;

  void Init(::duckdb::DataChunk &chunk, ::duckdb::idx_t col) {
    using ::duckdb::LogicalTypeId;
    using ::duckdb::PhysicalType;
    auto &vec = chunk.data[col];
    const auto &type = vec.GetType();
    kind = Kind::Other;
    switch (type.id()) {
    case LogicalTypeId::BOOLEAN: kind = Kind::Bool; break;
    case LogicalTypeId::TINYINT: kind = Kind::Int8; break;
    case LogicalTypeId::SMALLINT: kind = Kind::Int16; break;
    case LogicalTypeId::INTEGER: kind = Kind::Int32; break;
    case LogicalTypeId::BIGINT: kind = Kind::Int64; break;
    case LogicalTypeId::DATE: kind = Kind::Date; break;
    case LogicalTypeId::FLOAT: kind = Kind::Float; break;
    case LogicalTypeId::DOUBLE: kind = Kind::Double; break;
    case LogicalTypeId::VARCHAR: kind = Kind::Varchar; break;
    case LogicalTypeId::DECIMAL:
      scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(type));
      switch (type.InternalType()) {
      case PhysicalType::INT16: kind = Kind::Dec16; break;
      case PhysicalType::INT32: kind = Kind::Dec32; break;
      case PhysicalType::INT64: kind = Kind::Dec64; break;
      case PhysicalType::INT128: kind = Kind::Dec128; break;
      default: break;
      }
      break;
    default: break;
    }
    if (kind != Kind::Other)
      vec.ToUnifiedFormat(chunk.size(), uvf);
  }

  void Get(const ::duckdb::DataChunk &chunk, ::duckdb::idx_t col, ::duckdb::idx_t r, core::AuxValue &out) const {
    using core::AuxType;
    const auto i = uvf.sel->get_index(r);
    const bool valid = kind == Kind::Other ? true : uvf.validity.RowIsValid(i);
    const auto set = [&](AuxType t, auto v) {
      out.type = t;
      out.is_null = false;
      out.payload = std::move(v);
    };
    const auto null = [&](AuxType t) {
      out.type = t;
      out.is_null = true;
      out.payload = std::string();
    };
    switch (kind) {
    case Kind::Bool:
      return valid ? set(AuxType::Boolean, static_cast<bool>(::duckdb::UnifiedVectorFormat::GetData<bool>(uvf)[i]))
                   : null(AuxType::Boolean);
    case Kind::Int8:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int8_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int16:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int16_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int32:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int32_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Int64:
      return valid ? set(AuxType::Int64, static_cast<int64_t>(::duckdb::UnifiedVectorFormat::GetData<int64_t>(uvf)[i]))
                   : null(AuxType::Int64);
    case Kind::Date:
      return valid ? set(AuxType::Date, core::AuxDate{::duckdb::UnifiedVectorFormat::GetData<::duckdb::date_t>(uvf)[i].days})
                   : null(AuxType::Date);
    case Kind::Float:
      return valid ? set(AuxType::Double, static_cast<double>(::duckdb::UnifiedVectorFormat::GetData<float>(uvf)[i]))
                   : null(AuxType::Double);
    case Kind::Double:
      return valid ? set(AuxType::Double, ::duckdb::UnifiedVectorFormat::GetData<double>(uvf)[i])
                   : null(AuxType::Double);
    case Kind::Dec16:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int16_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec32:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int32_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec64:
      return valid ? set(AuxType::ScaledDecimal, core::ScaledDecimal{::duckdb::UnifiedVectorFormat::GetData<int64_t>(uvf)[i], scale})
                   : null(AuxType::ScaledDecimal);
    case Kind::Dec128: {
      if (!valid)
        return null(AuxType::ScaledDecimal);
      const auto huge = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::hugeint_t>(uvf)[i];
      const bool fits = (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
                        (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
      if (!fits) // beyond 64 bits: the boxed path's exact decimal text
        out = CapturedValue(chunk.GetValue(col, r));
      else
        set(AuxType::ScaledDecimal, core::ScaledDecimal{static_cast<int64_t>(huge.lower), scale});
      return;
    }
    case Kind::Varchar: {
      if (!valid)
        return null(AuxType::String);
      const auto &str = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(uvf)[i];
      return set(AuxType::String, std::string(str.GetData(), str.GetSize()));
    }
    case Kind::Other:
      out = CapturedValue(chunk.GetValue(col, r));
      return;
    }
  }
};

// The kit's engine services on a runtime's connection: SQL as the
// connection's transaction sees it (a writer's own writes included).
struct DuckVerusHost final : verus_kit::Host {
  explicit DuckVerusHost(::duckdb::Connection &connection_p) : connection(connection_p) {}

  // Chunk at a time: each column is read through its UnifiedVectorFormat
  // (flat, constant or dictionary), no Value boxing per cell. The produced
  // AuxValues are exactly CapturedValue(chunk.GetValue(c, r)).
  static void DeliverChunks(::duckdb::QueryResult &result, const verus_kit::RowFn &row) {
    Require(!result.HasError(), result.GetError());
    std::vector<ChunkColumn> columns;
    while (true) {
      auto chunk = result.Fetch();
      if (!chunk || chunk->size() == 0)
        break;
      const auto n = chunk->size();
      columns.resize(chunk->ColumnCount());
      for (::duckdb::idx_t c = 0; c < columns.size(); ++c)
        columns[c].Init(*chunk, c);
      core::AuxRow out;
      for (::duckdb::idx_t r = 0; r < n; ++r) {
        out.values.resize(columns.size());
        for (::duckdb::idx_t c = 0; c < columns.size(); ++c)
          columns[c].Get(*chunk, c, r, out.values[c]);
        row(out);
      }
    }
    Require(!result.HasError(), result.GetError());
  }

  void Query(const std::string &sql, const verus_kit::RowFn &row) override {
    auto lock = LockScan(connection);
    auto result = ScanConnection(connection).SendQuery(sql);
    Require(result != nullptr, "DuckDB scan returned null");
    DeliverChunks(*result, row);
  }

  bool QueryBigintList(const std::string &sql, const std::vector<int64_t> &list,
                       const verus_kit::RowFn &row) override {
    auto lock = LockScan(connection);
    auto stmt = ScanConnection(connection).Prepare(sql);
    Require(stmt && !stmt->HasError(), stmt ? stmt->GetError() : "prepare failed: " + sql);
    ::duckdb::vector<::duckdb::Value> items;
    items.reserve(list.size());
    for (auto v : list)
      items.push_back(::duckdb::Value::BIGINT(v));
    ::duckdb::vector<::duckdb::Value> values;
    values.push_back(::duckdb::Value::LIST(::duckdb::LogicalType::BIGINT, std::move(items)));
    auto result = stmt->Execute(values, /*allow_stream_result=*/true);
    Require(result != nullptr, "query failed: " + sql);
    DeliverChunks(*result, row);
    return true;
  }

  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params,
                   const verus_kit::RowFn &row) override {
    auto scan_lock = LockScan(connection);
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
    auto scan_lock = LockScan(connection);
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
      Require(found >= 0, "table " + table + " has no column " + col +
                              " (a capture lists the table's own columns; compute a derived delta field, e.g. "
                              "`... AS " + col + "`, in the glue's Apply from the base columns it reads)");
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

// Active runtimes: a handful (one per registered accelerator), read on every
// query and written on build/teardown. A vector with a shared lock: a lookup
// allocates nothing and concurrent readers do not serialize.
struct ActiveRuntimeEntry {
  ::duckdb::DatabaseInstance *db = nullptr;
  std::string accelerator_id;
  DuckdbRuntime *runtime = nullptr;
};

std::shared_mutex &ActiveRuntimeMutex() {
  static std::shared_mutex mutex;
  return mutex;
}

std::vector<ActiveRuntimeEntry> &ActiveRuntimes() {
  static std::vector<ActiveRuntimeEntry> runtimes;
  return runtimes;
}

void RegisterActiveRuntime(DuckdbRuntime *runtime,
                           ::duckdb::Connection &connection,
                           const SharedQueryCatalogEntry &entry) {
  if (!runtime || !entry.accelerator_id || !connection.context ||
      !connection.context->db) {
    return;
  }
  auto *db = connection.context->db.get();
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (auto &active : ActiveRuntimes()) {
    if (active.db == db && active.accelerator_id == entry.accelerator_id) {
      active.runtime = runtime;
      return;
    }
  }
  ActiveRuntimes().push_back({db, entry.accelerator_id, runtime});
}

void UnregisterActiveRuntime(DuckdbRuntime *runtime) {
  if (!runtime) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  auto &runtimes = ActiveRuntimes();
  runtimes.erase(std::remove_if(runtimes.begin(), runtimes.end(),
                                [&](const ActiveRuntimeEntry &e) { return e.runtime == runtime; }),
                 runtimes.end());
}

DuckdbRuntime *FindActiveRuntimeImpl(::duckdb::DatabaseInstance &db,
                                     const std::string &accelerator_id) {
  std::shared_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (const auto &active : ActiveRuntimes()) {
    if (active.db == &db && active.accelerator_id == accelerator_id) {
      return active.runtime;
    }
  }
  return nullptr;
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
  auto scan_lock = LockScan(connection);
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
  // Every update of a watched table is reported, also one that changes no
  // watched column: a key-only (lossy) delta is a "row with this key changed"
  // signal (its kernel rescans the key), so dropping it loses the change. For a
  // full-row delta the old and new projections are equal and cancel.
  bool TryGetUpdateOldColumns(::duckdb::TableCatalogEntry &table,
                              const ::duckdb::vector<::duckdb::PhysicalIndex> &,
                              ::duckdb::vector<::duckdb::PhysicalIndex> &old_columns) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = watched.find(table.name);
    if (it == watched.end()) {
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

// One consumer per connection: converts each mutation event's values once and
// hands the converted event to every runtime capturing on the connection, in
// registration order.  Events can arrive from parallel sink threads, so the
// runtime list is guarded and the conversion keeps no shared state.
class AuxCaptureHub final : public ::duckdb::MutationConsumer {
public:
  static constexpr const char *kKey = "accel_aux_capture_hub";

  void Add(DuckdbRuntime *runtime) {
    std::lock_guard<std::mutex> lock(mutex);
    runtimes.push_back(runtime);
  }
  // Returns true if no runtime is left.
  bool Remove(DuckdbRuntime *runtime) {
    std::lock_guard<std::mutex> lock(mutex);
    runtimes.erase(std::remove(runtimes.begin(), runtimes.end(), runtime), runtimes.end());
    return runtimes.empty();
  }

  void OnTableMutation(::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) override;

private:
  std::mutex mutex;
  std::vector<DuckdbRuntime *> runtimes;
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
    if (value.type().InternalType() == ::duckdb::PhysicalType::INT128) {
      const auto huge = value.GetValueUnsafe<::duckdb::hugeint_t>();
      const bool fits = (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
                        (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
      if (!fits) // beyond 64 bits: exact as decimal text (DuckDB parses it back exactly), never via double
        return core::AuxValue::Of(core::AuxType::String, value.ToString());
    }
    core::ScaledDecimal decimal;
    decimal.value = DecimalRaw(value);
    decimal.scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(value.type()));
    return core::AuxValue::Of(core::AuxType::ScaledDecimal, decimal);
  }
  case LogicalTypeId::VARCHAR:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, ::duckdb::StringValue::Get(value));
  default:
    return value.IsNull() ? core::AuxValue::Null(core::AuxType::String)
                          : core::AuxValue::Of(core::AuxType::String, value.ToString());
  }
}

// Converts column `col` of an event (`rows` rows, vector `vec`) into the row-major `out` (row stride `width`),
// reading the vector's physical data directly. Produces the same AuxValue as CapturedValue(vec.GetValue(r)) for
// every row; types without a direct path go through that.
void CapturedColumn(::duckdb::Vector &vec, std::size_t rows, std::vector<core::AuxValue> &out,
                    std::size_t width, std::size_t col) {
  using ::duckdb::LogicalTypeId;
  using ::duckdb::PhysicalType;
  ::duckdb::UnifiedVectorFormat fmt;
  vec.ToUnifiedFormat(rows, fmt);
  const auto &type = vec.GetType();
  auto at = [&](std::size_t r) -> core::AuxValue & { return out[r * width + col]; };
  auto fill = [&](core::AuxType aux_type, auto &&get) {
    for (std::size_t r = 0; r < rows; ++r) {
      const auto idx = fmt.sel->get_index(r);
      auto &dst = at(r);
      dst.type = aux_type;
      if (!fmt.validity.RowIsValid(idx)) {
        dst.is_null = true;
        continue;
      }
      dst.is_null = false;
      dst.payload = get(idx);
    }
  };
  auto fill_null = [&](core::AuxType aux_type) {
    for (std::size_t r = 0; r < rows; ++r) {
      auto &dst = at(r);
      dst.type = aux_type;
      dst.is_null = true;
    }
  };
  switch (type.id()) {
  case LogicalTypeId::BOOLEAN: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<bool>(fmt);
    fill(core::AuxType::Boolean, [&](::duckdb::idx_t i) -> bool { return data[i]; });
    return;
  }
  case LogicalTypeId::TINYINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int8_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::SMALLINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int16_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::INTEGER: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::BIGINT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int64_t>(fmt);
    fill(core::AuxType::Int64, [&](::duckdb::idx_t i) -> int64_t { return data[i]; });
    return;
  }
  case LogicalTypeId::DATE: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::date_t>(fmt);
    fill(core::AuxType::Date, [&](::duckdb::idx_t i) { return core::AuxDate {::duckdb::Date::EpochDays(data[i])}; });
    return;
  }
  case LogicalTypeId::FLOAT: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<float>(fmt);
    fill(core::AuxType::Double, [&](::duckdb::idx_t i) -> double { return static_cast<double>(data[i]); });
    return;
  }
  case LogicalTypeId::DOUBLE: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<double>(fmt);
    fill(core::AuxType::Double, [&](::duckdb::idx_t i) -> double { return data[i]; });
    return;
  }
  case LogicalTypeId::DECIMAL: {
    const auto scale = static_cast<int32_t>(::duckdb::DecimalType::GetScale(type));
    auto scaled = [&](int64_t raw) {
      core::ScaledDecimal decimal;
      decimal.value = raw;
      decimal.scale = scale;
      return decimal;
    };
    switch (type.InternalType()) {
    case PhysicalType::INT16: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int16_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT32: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int32_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT64: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<int64_t>(fmt);
      fill(core::AuxType::ScaledDecimal, [&](::duckdb::idx_t i) { return scaled(data[i]); });
      return;
    }
    case PhysicalType::INT128: {
      const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::hugeint_t>(fmt);
      for (std::size_t r = 0; r < rows; ++r) {
        const auto idx = fmt.sel->get_index(r);
        auto &dst = at(r);
        if (!fmt.validity.RowIsValid(idx)) {
          dst = core::AuxValue::Null(core::AuxType::ScaledDecimal);
          continue;
        }
        const auto huge = data[idx];
        const bool fits = (huge.upper == 0 && static_cast<int64_t>(huge.lower) >= 0) ||
                          (huge.upper == -1 && static_cast<int64_t>(huge.lower) < 0);
        if (fits) {
          dst = core::AuxValue::Of(core::AuxType::ScaledDecimal, scaled(static_cast<int64_t>(huge.lower)));
        } else { // beyond 64 bits: exact as decimal text, never via double
          dst = CapturedValue(vec.GetValue(r));
        }
      }
      return;
    }
    default:
      break;
    }
    break;
  }
  case LogicalTypeId::VARCHAR: {
    const auto *data = ::duckdb::UnifiedVectorFormat::GetData<::duckdb::string_t>(fmt);
    fill(core::AuxType::String, [&](::duckdb::idx_t i) { return data[i].GetString(); });
    return;
  }
  case LogicalTypeId::SQLNULL:
    fill_null(core::AuxType::String);
    return;
  default:
    break;
  }
  for (std::size_t r = 0; r < rows; ++r) {
    at(r) = CapturedValue(vec.GetValue(r));
  }
}

void AuxCaptureHub::OnTableMutation(::duckdb::ClientContext &, const ::duckdb::TableMutationEvent &event) {
  if (event.row_count == 0) {
    return;
  }
  std::vector<DuckdbRuntime *> targets;
  {
    std::lock_guard<std::mutex> lock(mutex);
    targets = runtimes;
  }
  if (targets.empty()) {
    return;
  }
  DuckdbRuntime::CapturedEvent shared;
  shared.table_name = event.table_name;
  shared.kind = event.kind == ::duckdb::TableMutationKind::INSERT   ? core::MutationKind::Insert
                : event.kind == ::duckdb::TableMutationKind::DELETE ? core::MutationKind::Delete
                                                                    : core::MutationKind::Update;
  shared.updated_columns.reserve(event.updated_columns.size());
  for (const auto &u : event.updated_columns) {
    shared.updated_columns.push_back(static_cast<int32_t>(u.index));
  }
  // Every row of an event has the same shape (see MutationRecorder).
  shared.rows = event.row_count;
  shared.width = event.columns.size();
  shared.olds = shared.kind == core::MutationKind::Update ? event.old_count : shared.width;
  shared.columns.reserve(shared.width);
  for (const auto &c : event.columns) {
    shared.columns.push_back(static_cast<int32_t>(c.index));
  }
  shared.values.resize(shared.rows * shared.width);
  for (std::size_t c = 0; c < shared.width; ++c) {
    CapturedColumn(*event.vectors[c], shared.rows, shared.values, shared.width, c);
  }
  for (auto *runtime : targets) {
    runtime->OnCapturedEvent(shared);
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
  connection_.context->registered_state->GetOrCreate<AuxCaptureHub>(AuxCaptureHub::kKey)->Add(this);
  capture_enabled_ = true;
}

void DuckdbRuntime::DisableMutationCapture() {
  if (!capture_enabled_) {
    return;
  }
  if (auto hub = connection_.context->registered_state->Get<AuxCaptureHub>(AuxCaptureHub::kKey)) {
    if (hub->Remove(this)) {
      connection_.context->registered_state->Remove(AuxCaptureHub::kKey);
    }
  }
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

void DuckdbRuntime::OnCapturedEvent(const CapturedEvent &event) {
  constexpr std::size_t kNone = static_cast<std::size_t>(-1);
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
  const bool is_update = event.kind == core::MutationKind::Update;
  std::vector<std::size_t> old_pos, new_pos;
  for (const auto &dep : binding_->Module().Dependencies()) {
    if (dep.table != table_id || dep.mutation_attnums.empty()) {
      continue;
    }
    const std::size_t n = dep.mutation_attnums.size();
    core::MutationBatch batch;
    batch.table = table_id;
    batch.kind = event.kind;
    if (is_update) {
      // Delivered even when no captured column changed (see TryGetUpdateOldColumns):
      // the old and new projections then are equal -- a key-only delta's "touched".
      for (std::size_t j = 0; j < n; ++j) {
        for (auto u : event.updated_columns) {
          if (u == dep.mutation_attnums[j]) {
            batch.updated_columns.push_back(static_cast<core::ColumnId>(j));
          }
        }
      }
    }
    // Position of each projected column among the event's values: the old
    // values first (a later duplicate wins), then the updated columns' new
    // values; the new row is the old one with those replaced.
    old_pos.assign(n, kNone);
    new_pos.assign(n, kNone);
    for (std::size_t j = 0; j < n; ++j) {
      for (std::size_t i = 0; i < event.width; ++i) {
        if (event.columns[i] == dep.mutation_attnums[j]) {
          (is_update && i >= event.olds ? new_pos : old_pos)[j] = i;
        }
      }
    }
    auto project = [&](std::size_t r, bool use_new) {
      core::AuxRow out;
      out.values.reserve(n);
      const core::AuxValue *base = event.values.data() + r * event.width;
      for (std::size_t j = 0; j < n; ++j) {
        std::size_t pos = use_new ? new_pos[j] : kNone;
        if (pos == kNone) {
          pos = old_pos[j];
        }
        out.values.push_back(pos == kNone ? core::AuxValue::Null(core::AuxType::String) : base[pos]);
      }
      return out;
    };
    switch (event.kind) {
    case core::MutationKind::Insert:
      batch.new_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.new_rows.push_back(project(r, false));
      }
      break;
    case core::MutationKind::Delete:
      batch.old_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.old_rows.push_back(project(r, false));
      }
      break;
    case core::MutationKind::Update:
      batch.old_rows.reserve(event.rows);
      batch.new_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.old_rows.push_back(project(r, false));
        batch.new_rows.push_back(project(r, true));
      }
      break;
    }
    if (!batch.old_rows.empty() || !batch.new_rows.empty()) {
      backend_.CaptureMutation(batch);
    }
  }
}

void DuckdbRuntime::BeginWrite() { connection_.BeginTransaction(); }

uint64_t DuckdbRuntime::ApplyWrite(const std::string &sql) {
  BeginWrite();
  // statement by statement (one transaction), so each INSERT / UPDATE / DELETE reports its row count
  uint64_t rows = 0;
  try {
    for (auto &statement : connection_.ExtractStatements(sql)) {
      auto result = connection_.Query(std::move(statement));
      if (!result || result->HasError()) {
        throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
      }
      if (result->ColumnCount() == 1 && result->RowCount() == 1 && result->names[0] == "Count") {
        rows += static_cast<uint64_t>(result->GetValue(0, 0).GetValue<int64_t>());
      }
    }
  } catch (...) {
    RollbackWrite();
    throw;
  }
  CommitWrite();
  return rows;
}

void DuckdbRuntime::ApplyWriteAll(const std::vector<DuckdbRuntime *> &runtimes, const std::string &sql,
                                  WriteAllTiming *timing) {
  if (runtimes.empty())
    throw std::invalid_argument("ApplyWriteAll: no runtimes");
  auto &connection = runtimes.front()->connection_;
  for (auto *rt : runtimes)
    if (&rt->connection_ != &connection)
      throw std::invalid_argument("ApplyWriteAll: runtimes on different connections");
  const auto abort_all = [&] {
    for (auto *rt : runtimes)
      rt->coordinator_.Abort();
    try {
      if (connection.HasActiveTransaction())
        connection.Rollback();
    } catch (...) {
    }
  };
  using TimingClock = std::chrono::steady_clock;
  const auto us = [](TimingClock::time_point a, TimingClock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };
  if (timing)
    *timing = WriteAllTiming{};
  auto t0 = TimingClock::now();
  connection.BeginTransaction();
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    abort_all();
    throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
  }
  if (timing)
    timing->statement_us = us(t0, TimingClock::now());
  // as CommitWrite, for every runtime: one clock (per database), each
  // coordinator's PRE_COMMIT, the engine commit, each one's COMMIT
  runtimes.front()->clock_->epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context) + 1;
  try {
    // PRE_COMMIT of every runtime. Each coordinator, backend, module and
    // state store is its own; what they share is the writer's connection
    // (every scan serializes on it, see scan::LockScan), the Verus world
    // store (internally locked; WorldShared::Begin counts participants under
    // its mutex) and the decision log (thread-local per runtime here).
    // ACCEL_PRECOMMIT_THREADS=1 is the serial loop.
    std::size_t threads = std::min<std::size_t>(runtimes.size(), std::max(1u, std::thread::hardware_concurrency()));
    if (const char *env = std::getenv("ACCEL_PRECOMMIT_THREADS"); env && *env) {
      const long parsed = std::strtol(env, nullptr, 10);
      if (parsed > 0)
        threads = std::min<std::size_t>(runtimes.size(), static_cast<std::size_t>(parsed));
    }
    if (threads <= 1) {
      for (auto *rt : runtimes) {
        if (timing)
          core::ClearMaintenanceDecisionLog();
        t0 = TimingClock::now();
        rt->coordinator_.PreCommit();
        if (timing) {
          timing->pre_commit_us.push_back(us(t0, TimingClock::now()));
          const auto d = core::LastMaintenanceDecision();
          timing->action.push_back(d ? maintenance::MaintenanceActionName(d->staged_action) : "none");
          timing->decision.push_back(d ? *d : core::MaintenanceDecisionLogEntry{});
        }
      }
    } else {
      const std::size_t n = runtimes.size();
      std::vector<double> pre_us(n, 0.0);
      std::vector<std::optional<core::MaintenanceDecisionLogEntry>> decisions(n);
      std::vector<std::exception_ptr> errors(n);
      std::atomic<std::size_t> next{0};
      std::atomic<bool> failed{false};
      const auto work = [&] {
        for (;;) {
          const std::size_t i = next.fetch_add(1);
          if (i >= n || failed.load())
            return;
          try {
            core::ScopedThreadMaintenanceDecisionLog scoped_log;
            const auto start = TimingClock::now();
            runtimes[i]->coordinator_.PreCommit();
            pre_us[i] = us(start, TimingClock::now());
            decisions[i] = core::LastMaintenanceDecision();
          } catch (...) {
            errors[i] = std::current_exception();
            failed = true;
            return;
          }
        }
      };
      std::vector<std::thread> pool;
      pool.reserve(threads - 1);
      try {
        for (std::size_t k = 1; k < threads; ++k)
          pool.emplace_back(work);
      } catch (...) {
        failed = true; // could not start a thread: let the started ones finish, then abort
        for (auto &t : pool)
          t.join();
        throw;
      }
      work();
      for (auto &t : pool)
        t.join();
      for (auto &e : errors)
        if (e)
          std::rethrow_exception(e);
      if (timing) {
        for (std::size_t i = 0; i < n; ++i) {
          const auto &d = decisions[i];
          timing->pre_commit_us.push_back(pre_us[i]);
          timing->action.push_back(d ? maintenance::MaintenanceActionName(d->staged_action) : "none");
          timing->decision.push_back(d ? *d : core::MaintenanceDecisionLogEntry{});
        }
      }
      // what the serial loop leaves in the process-wide log: the last runtime's
      core::ClearMaintenanceDecisionLog();
      if (decisions[n - 1])
        core::SetGlobalMaintenanceDecision(*decisions[n - 1]);
    }
    t0 = TimingClock::now();
    connection.Commit();
    if (timing)
      timing->engine_commit_us = us(t0, TimingClock::now());
  } catch (...) {
    abort_all();
    throw;
  }
  // COMMIT (install) of every runtime. The engine commit is already visible,
  // so every runtime must run its install even if another throws: a skipped
  // one would stay "pending" and block newest-snapshot readers forever. Each
  // install touches only its own coordinator/store (FinishCommit appends one
  // version to that accelerator's list and clears its pending flag); all
  // installs publish at the same, already fixed, commit epoch, so install order
  // is not observable. The Verus world store assembles its batch under its own
  // mutex (last finisher installs). ACCEL_COMMIT_THREADS=1 is the serial loop.
  const std::size_t n = runtimes.size();
  std::size_t threads = std::min<std::size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
  for (const char *name : {"ACCEL_COMMIT_THREADS", "ACCEL_PRECOMMIT_THREADS"}) {
    if (const char *env = std::getenv(name); env && *env) {
      const long parsed = std::strtol(env, nullptr, 10);
      if (parsed > 0) {
        threads = std::min<std::size_t>(n, static_cast<std::size_t>(parsed));
        break;
      }
    }
  }
  if (threads <= 1) {
    for (auto *rt : runtimes) {
      t0 = TimingClock::now();
      rt->coordinator_.Commit();
      if (timing)
        timing->commit_us.push_back(us(t0, TimingClock::now()));
    }
    return;
  }
  std::vector<double> commit_us(n, 0.0), maint_us(n, 0.0);
  std::vector<std::exception_ptr> errors(n);
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (;;) {
      const std::size_t i = next.fetch_add(1);
      if (i >= n)
        return;
      try {
        core::ScopedThreadMaintenanceDecisionLog scoped_log;
        const auto start = TimingClock::now();
        runtimes[i]->coordinator_.Commit();
        commit_us[i] = us(start, TimingClock::now());
        if (const auto d = core::LastMaintenanceDecision())
          maint_us[i] = d->maintenance_us;
      } catch (...) {
        errors[i] = std::current_exception();
      }
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  try {
    for (std::size_t k = 1; k < threads; ++k)
      pool.emplace_back(work);
  } catch (...) {
    // could not start a thread: the ones started plus this thread still drain all work
  }
  work();
  for (auto &t : pool)
    t.join();
  if (timing)
    timing->commit_us.insert(timing->commit_us.end(), commit_us.begin(), commit_us.end());
  // the process-wide log keeps what the serial loop leaves: the last staged runtime's
  for (std::size_t i = n; i-- > 0;)
    if (maint_us[i] > 0.0) {
      core::RecordCoordinatorMaintenanceUs(maint_us[i]);
      break;
    }
  for (auto &e : errors)
    if (e)
      std::rethrow_exception(e);
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
                     harness::SharedCostModelConfigPath(entry));
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
    // leave no open (or aborted) engine transaction behind: the next write starts clean
    try {
      if (connection_.HasActiveTransaction())
        connection_.Rollback();
    } catch (...) {
    }
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
    // Readable now: fill the table function's schema cache so the first read
    // doesn't plan the base SQL.
    ::accelerator_codegen::aux_state::engine::PrimeAuxSchema(connection_, entry_);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

void DuckdbRuntime::ExecuteInto(const core::QueryArguments &arguments, core::AuxRowSink &sink) {
  AuxReadScope one_version; // every read in this query: one state version
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks,
                     static_cast<::duckdb::transaction_t>(DuckDBAuxBackend::SnapshotEpoch(*reader)));
  }
  t_table_scans = 0;
  binding_->Execute(backend_, arguments, sink);
  core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                           : core::AuxExecutePath::Hit);
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  AuxReadScope one_version; // every read in this query: one state version
  // Normally unused: commits publish valid state (ReadersCanBuildState).
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks,
                     static_cast<::duckdb::transaction_t>(DuckDBAuxBackend::SnapshotEpoch(*reader)));
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

namespace {

// Common envelope of a timed maintenance write: fresh decision log, wall time
// of `write` (begin ... commit) recorded as the actual maintenance cost.
template <typename Write> void TimedMaintenanceWrite(Write &&write) {
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  write();
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
}

// Runs one write statement inside the open transaction; rolls back and throws on error.
void RunWriteQuery(::duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  if (result == nullptr || result->HasError()) {
    const auto error = result ? result->GetError() : "DuckDB write failed";
    connection.Rollback();
    Require(false, error);
  }
}

} // namespace

void DuckdbRuntime::Apply(SharedProbe &probe) {
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    RunWriteQuery(connection_, ProbeWriteSql(probe));
    backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
    CommitWrite();
    AdvanceProbe(probe);
  });
}

void DuckdbRuntime::ApplyBatch(std::vector<SharedProbe> &probes) {
  Require(!probes.empty(), "ApplyBatch requires at least one probe");
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    for (auto &probe : probes) {
      RunWriteQuery(connection_, ProbeWriteSql(probe));
      backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
      AdvanceProbe(probe);
    }
    CommitWrite();
  });
}

void DuckdbRuntime::ApplyBatchWrite(const std::string &sql,
                                    const core::MutationBatch &mutation) {
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    RunWriteQuery(connection_, sql);
    backend_.CaptureMutation(mutation);
    CommitWrite();
  });
}

void DuckdbRuntime::ApplyRfChunk(int write_seq, int order_count) {
  if (entry_.workload != SharedWorkloadKind::Tpch) {
    throw std::runtime_error("ApplyRfChunk is only supported for TPC-H");
  }
  // A Verus kit's capture maps the SQL's own mutations to its tables by name
  // (the hand-built batches below carry the C++ bindings' table ids and
  // column layouts): run the chunk as SQL through the captured write path.
  // Likewise a C++ accelerator the hand-built batches do not cover (no case in
  // TpchRfTableIdsForQuery): its module receives the SQL's real mutations,
  // projected as it declares, instead of none at all.
  const auto rf_ids = TpchRfTableIdsForQuery(entry_.query_id);
  if (verus_kit::Find(entry_.accelerator_id) ||
      (!rf_ids.watches_orders && !rf_ids.watches_lineitem && entry_.query_id != 11)) {
    // capture on for this chunk only: the hand-built probe writes (Apply)
    // deliver their own mutation and must not be captured twice
    const bool was_capturing = capture_enabled_;
    EnableMutationCapture();
    try {
      ApplyWrite(TpchRfChunkStatements(connection_, NextTpchRfChunkPlan(write_seq, order_count, connection_)));
    } catch (...) {
      if (!was_capturing)
        DisableMutationCapture();
      throw;
    }
    if (!was_capturing)
      DisableMutationCapture();
    return;
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
  // A Verus kit's SQL is parameterized ($1, ...): its own fallback runs it
  // with the benchmark arguments.
  if (verus_kit::Find(entry_.accelerator_id)) {
    MaterializeSink sink;
    binding_->Module().ExecuteFallback(backend_, ToCoreArguments(entry_.benchmark_arguments), sink);
    return;
  }
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
