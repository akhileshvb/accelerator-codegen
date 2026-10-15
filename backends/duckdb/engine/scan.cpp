// GENERATED BACKEND
#include "backends/duckdb/engine/scan.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

#include "duckdb/accelerator/snapshot_override.hpp"
#include "duckdb/common/types/value.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

ScanRedirect::ScanRedirect(::duckdb::DatabaseInstance &db_p, std::atomic<uint64_t> &opened_p,
                           ::duckdb::transaction_t snapshot_p)
    : db(db_p), opened(opened_p), snapshot(snapshot_p), previous(current) {
  current = this;
}

ScanRedirect::~ScanRedirect() {
  current = previous;
  if (connection && snapshot)
    ::duckdb::ClearTransactionStartOverride(*connection->context);
}

::duckdb::Connection &ScanRedirect::Connection() {
  if (!connection) {
    connection = std::make_unique<::duckdb::Connection>(db);
    if (snapshot)
      ::duckdb::SetTransactionStartOverride(*connection->context, snapshot);
    opened++;
  }
  return *connection;
}

thread_local ScanRedirect *ScanRedirect::current = nullptr;

thread_local uint64_t t_table_scans = 0;
thread_local const core::AuxEngineServices *scan::t_scan_services = nullptr;

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

} // namespace accelerator_codegen::aux_state::engine::duckdb
