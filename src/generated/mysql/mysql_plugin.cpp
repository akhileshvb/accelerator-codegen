// Generic query-UDF implementation + observer/maintenance UDFs for the MySQL plugin.
// Query UDF symbols live in each query's dir (see udf_shim.hpp).
#include "accelerator_codegen/mysql/plugin_minimal.h"
#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_exact_parse.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace core = accelerator_codegen::aux_state::core;
namespace mysql_rt = accelerator_codegen::aux_state::engine::mysql;

namespace {

// The result column's declared maximum (LONGTEXT-sized). The result itself
// lives in a growing std::string, so no result is ever truncated; one larger
// than the server's max_allowed_packet fails loudly in the server.
constexpr unsigned long kMaxResultDeclared = 0x7fffffffUL;

std::string ArgString(UDF_ARGS *args, unsigned idx) {
  if (!args || idx >= args->arg_count || !args->args[idx]) {
    return {};
  }
  if (args->arg_type && args->arg_type[idx] == INT_RESULT) {
    return std::to_string(*reinterpret_cast<long long *>(args->args[idx]));
  }
  if (args->arg_type && args->arg_type[idx] == REAL_RESULT) {
    return std::to_string(*reinterpret_cast<double *>(args->args[idx]));
  }
  if (args->lengths) {
    return std::string(args->args[idx], args->lengths[idx]);
  }
  return std::string(args->args[idx]);
}

namespace exact = accelerator_codegen::aux_state::engine::mysql::exact;

// A UDF argument as raw bytes; false for SQL NULL. (Init coerces every
// argument to STRING_RESULT, so the server hands over each one as its text:
// a DECIMAL literal 0.065 arrives as "0.065", an exact decimal string.)
bool ArgBytes(UDF_ARGS *args, unsigned idx, const char *&data, std::size_t &len) {
  if (!args || idx >= args->arg_count || !args->args[idx]) {
    return false;
  }
  data = args->args[idx];
  len = args->lengths ? args->lengths[idx] : std::strlen(args->args[idx]);
  return true;
}

core::AuxType ArgAuxType(char code) {
  switch (code) {
  case 'i': return core::AuxType::Int64;
  case 'f': return core::AuxType::Double;
  case 'n': return core::AuxType::ScaledDecimal;
  case 'd': return core::AuxType::Date;
  default: return core::AuxType::String;
  }
}

// Parses argument `idx` as type `code`. A NULL argument becomes a NULL
// AuxValue (never a default). Returns false with `error` set if the text is
// not a valid value of the type (never a silent 0 / 1970-01-01).
bool ParseOneArg(UDF_ARGS *args, unsigned idx, char code, core::AuxValue &out,
                 std::string &error) {
  const core::AuxType type = ArgAuxType(code);
  const char *data = nullptr;
  std::size_t len = 0;
  if (!ArgBytes(args, idx, data, len)) {
    out = core::AuxValue::Null(type);
    return true;
  }
  auto fail = [&](const char *what) {
    error = "argument " + std::to_string(idx + 1) + ": '" + std::string(data, std::min<std::size_t>(len, 64)) +
            "' is not a valid " + what;
    return false;
  };
  switch (code) {
  case 'i': {
    int64_t v = 0;
    if (!exact::ParseInt64(data, len, v)) {
      // An integral decimal ("90.0", "9e1") is an integer too.
      core::ScaledDecimal d;
      if (!exact::ParseDecimal(data, len, d) || !exact::Rescale(d, 0, v)) {
        return fail("integer");
      }
    }
    out = core::AuxValue::Of(core::AuxType::Int64, v);
    return true;
  }
  case 'f': {
    const std::string text(data, len);
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(text.c_str(), &end);
    if (len == 0 || end != text.c_str() + text.size() || !std::isfinite(v) ||
        std::isspace(static_cast<unsigned char>(text[0]))) {
      return fail("number");
    }
    out = core::AuxValue::Of(core::AuxType::Double, v);
    return true;
  }
  case 'n': {
    core::ScaledDecimal d;
    if (!exact::ParseDecimal(data, len, d)) {
      return fail("decimal");
    }
    out = core::AuxValue::Of(core::AuxType::ScaledDecimal, d);
    return true;
  }
  case 'd': {
    int32_t days = 0;
    if (!exact::ParseDate(data, len, days)) {
      return fail("date (YYYY-MM-DD)");
    }
    out = core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days});
    return true;
  }
  default:
    out = core::AuxValue::Of(core::AuxType::String, std::string(data, len));
    return true;
  }
}

bool ParseTypedArgs(UDF_ARGS *args, const char *types, core::QueryArguments &out,
                    std::string &error) {
  unsigned i = 0;
  for (const char *t = types; *t; ++t, ++i) {
    core::AuxValue value;
    if (!ParseOneArg(args, i, *t, value, error)) {
      return false;
    }
    out.values.push_back(std::move(value));
  }
  return true;
}

// Owned by a UDF call site (UDF_INIT::ptr): the result text, grown as needed.
struct ResultBuffer {
  std::string text;
};

bool InitStringUdf(UDF_INIT *initid, UDF_ARGS *args, unsigned expected) {
  if (args->arg_count != expected) {
    return true;
  }
  if (args->arg_type) {
    for (unsigned i = 0; i < args->arg_count; ++i) {
      args->arg_type[i] = STRING_RESULT;
    }
  }
  initid->maybe_null = 1; // a failed call (bad argument, query error) is NULL
  initid->max_length = kMaxResultDeclared;
  initid->ptr = reinterpret_cast<char *>(new ResultBuffer());
  return false;
}

void DeinitStringUdf(UDF_INIT *initid) {
  if (initid && initid->ptr) {
    delete reinterpret_cast<ResultBuffer *>(initid->ptr);
    initid->ptr = nullptr;
  }
}

// Hands `text` to the server. The server copies it out before the next call,
// so the buffer lives in the call site and is reused; no size limit here.
char *CopyResult(UDF_INIT *initid, std::string &&text, unsigned long *length,
                 unsigned char *is_null, unsigned char *error) {
  if (!initid || !initid->ptr) {
    *error = 1;
    *is_null = 1;
    *length = 0;
    return nullptr;
  }
  auto *buffer = reinterpret_cast<ResultBuffer *>(initid->ptr);
  buffer->text = std::move(text);
  *length = static_cast<unsigned long>(buffer->text.size());
  return buffer->text.data();
}

} // namespace

namespace accelerator_codegen::aux_state::engine::mysql {

// Generic query UDF implementation; the per-query extern "C" symbols
// (q<N>/mysql/plugin/udf.cpp, ACCEL_MYSQL_QUERY_UDF) call these.
bool MysqlUdfInit(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *name) {
  const MysqlUdfEntry *entry = FindMysqlUdf(name);
  if (!entry || args->arg_count != entry->arg_types.size()) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE, "%s argument count", name);
    return true;
  }
  // Constant text arguments are known now (before they are all coerced to
  // strings below): a malformed one is a real SQL error here instead of a NULL
  // result per row. Integer/real constants arrive binary and cannot be
  // malformed text.
  std::vector<bool> known_text(args->arg_count, false);
  for (unsigned i = 0; i < args->arg_count; ++i) {
    known_text[i] = args->args[i] && args->arg_type &&
                    (args->arg_type[i] == STRING_RESULT || args->arg_type[i] == DECIMAL_RESULT);
  }
  if (InitStringUdf(initid, args, static_cast<unsigned>(entry->arg_types.size()))) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE, "%s argument count", name);
    return true;
  }
  for (unsigned i = 0; i < args->arg_count; ++i) {
    if (!known_text[i]) {
      continue;
    }
    core::AuxValue value;
    std::string error;
    if (!ParseOneArg(args, i, entry->arg_types[i], value, error)) {
      std::snprintf(message, MYSQL_ERRMSG_SIZE, "%s: %s", name, error.c_str());
      DeinitStringUdf(initid);
      return true;
    }
  }
  return false;
}

void MysqlUdfDeinit(UDF_INIT *initid) { DeinitStringUdf(initid); }

// Failure contract: a malformed argument or a failing query yields SQL NULL
// (error flag set, reason in the server log), never a default result. A
// legitimately empty result is the empty string.
char *MysqlUdfCall(UDF_INIT *initid, UDF_ARGS *args, unsigned long *length,
                   unsigned char *is_null, unsigned char *error, const char *name) {
  try {
    const MysqlUdfEntry *entry = FindMysqlUdf(name);
    if (!entry) {
      throw std::runtime_error("no MySQL UDF registered");
    }
    if (entry->null_first_arg_yields_empty &&
        (args->arg_count < 1 || args->args[0] == nullptr)) {
      return CopyResult(initid, std::string(), length, is_null, error);
    }
    core::QueryArguments parsed;
    std::string arg_error;
    if (!ParseTypedArgs(args, entry->arg_types.c_str(), parsed, arg_error)) {
      throw std::runtime_error(arg_error);
    }
    return CopyResult(initid, entry->execute(parsed), length, is_null, error);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[accel] %s failed: %s\n", name, e.what());
  } catch (...) {
    std::fprintf(stderr, "[accel] %s failed: unknown error\n", name);
  }
  *error = 1;
  *is_null = 1;
  *length = 0;
  return nullptr;
}

} // namespace accelerator_codegen::aux_state::engine::mysql

extern "C" {

__attribute__((visibility("default")))
bool mysql_accel_observer_mutation_count_init(UDF_INIT *initid, UDF_ARGS *args,
                                              char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_observer_mutation_count takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  return false;
}

__attribute__((visibility("default")))
long long mysql_accel_observer_mutation_count(UDF_INIT *, UDF_ARGS *, char *,
                                              char *) {
  return mysql_rt::MysqlObserverMutationCount();
}

__attribute__((visibility("default")))
bool mysql_accel_reset_observer_init(UDF_INIT *initid, UDF_ARGS *args,
                                     char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_reset_observer takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

__attribute__((visibility("default")))
char *mysql_accel_reset_observer(UDF_INIT *, UDF_ARGS *, char *result,
                                 unsigned long *length, unsigned char *,
                                 unsigned char *) {
  mysql_rt::MysqlResetObserver();
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_set_force_maintenance_action_init(UDF_INIT *initid,
                                                   UDF_ARGS *args,
                                                   char *message) {
  if (args->arg_count != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_set_force_maintenance_action takes 1 argument");
    return true;
  }
  if (args->arg_type) {
    args->arg_type[0] = STRING_RESULT;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

__attribute__((visibility("default")))
char *mysql_accel_set_force_maintenance_action(UDF_INIT *, UDF_ARGS *args,
                                               char *result,
                                               unsigned long *length,
                                               unsigned char *,
                                               unsigned char *) {
  mysql_rt::MysqlSetForceMaintenanceAction(ArgString(args, 0).c_str());
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_invalidate_all_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_invalidate_all takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

// Marks every accelerator's aux state invalid: the next query rebuilds it
// from the tables (tests; recovery after out-of-band changes such as bulk
// loads that bypass the capture triggers).
__attribute__((visibility("default")))
char *mysql_accel_invalidate_all(UDF_INIT *, UDF_ARGS *, char *result,
                                 unsigned long *length, unsigned char *,
                                 unsigned char *) {
  mysql_rt::InvalidateAllMysqlAccelerators(/*transaction_open=*/false);
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_set_scan_database_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_set_scan_database takes 1 argument");
    return true;
  }
  if (args->arg_type) {
    args->arg_type[0] = STRING_RESULT;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

// Selects the database the accelerators' table scans read (default: the
// server's ACCEL_MYSQL_DB / MYSQL_DATABASE environment). Pair with
// mysql_accel_invalidate_all() to rebuild from it.
__attribute__((visibility("default")))
char *mysql_accel_set_scan_database(UDF_INIT *, UDF_ARGS *args, char *result,
                                    unsigned long *length, unsigned char *,
                                    unsigned char *) {
  mysql_rt::detail::SetScanDatabase(ArgString(args, 0));
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_set_scan_mode_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE, "mysql_accel_set_scan_mode takes 1 argument");
    return true;
  }
  if (args->arg_type) {
    args->arg_type[0] = STRING_RESULT;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

// 'cli' or 'client' (see SetScanMode); for comparisons and debugging.
__attribute__((visibility("default")))
char *mysql_accel_set_scan_mode(UDF_INIT *, UDF_ARGS *args, char *result,
                                unsigned long *length, unsigned char *,
                                unsigned char *) {
  mysql_rt::detail::SetScanMode(ArgString(args, 0));
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_get_build_stats_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE, "mysql_accel_get_build_stats takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 160;
  return false;
}

// How aux-state builds went: snapshot_builds, restart_builds, replayed_batches
// (writes that committed during a snapshot build and were replayed on it),
// build_retries.
__attribute__((visibility("default")))
char *mysql_accel_get_build_stats(UDF_INIT *, UDF_ARGS *, char *result,
                                  unsigned long *length, unsigned char *,
                                  unsigned char *) {
  const auto &s = mysql_rt::detail::GetBuildStats();
  const int n = std::snprintf(result, 255, "snapshot_builds=%llu restart_builds=%llu replayed_batches=%llu build_retries=%llu",
                              static_cast<unsigned long long>(s.snapshot_builds.load()),
                              static_cast<unsigned long long>(s.restart_builds.load()),
                              static_cast<unsigned long long>(s.replayed_batches.load()),
                              static_cast<unsigned long long>(s.build_retries.load()));
  *length = static_cast<unsigned long>(n);
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_get_last_maintenance_decision_init(UDF_INIT *initid, UDF_ARGS *args,
                                                    char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_get_last_maintenance_decision takes no arguments");
    return true;
  }
  initid->maybe_null = 1;
  initid->max_length = 128;
  return false;
}

// The last maintenance decision ("action=A rows=N partitions=M"), for
// calibration; NULL if none.
__attribute__((visibility("default")))
char *mysql_accel_get_last_maintenance_decision(UDF_INIT *, UDF_ARGS *, char *result,
                                                unsigned long *length, unsigned char *is_null,
                                                unsigned char *) {
  const auto text = accelerator_codegen::aux_state::core::FormatLastMaintenanceDecision();
  if (text.empty()) {
    *is_null = 1;
    *length = 0;
    return result;
  }
  const auto n = std::min<std::size_t>(text.size(), 255);
  std::memcpy(result, text.data(), n);
  *length = n;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_get_execute_counts_init(UDF_INIT *initid, UDF_ARGS *args,
                                         char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_get_execute_counts takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 128;
  return false;
}

// How accelerator queries were answered (hit / rebuilt_on_read / fallback).
__attribute__((visibility("default")))
char *mysql_accel_get_execute_counts(UDF_INIT *, UDF_ARGS *, char *result,
                                     unsigned long *length, unsigned char *,
                                     unsigned char *) {
  const auto text = accelerator_codegen::aux_state::core::FormatAuxExecuteCounts(
      accelerator_codegen::aux_state::core::ReadAuxExecuteCounts());
  const auto n = std::min<std::size_t>(text.size(), 255);
  std::memcpy(result, text.data(), n);
  *length = n;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_set_expected_queries_init(UDF_INIT *initid, UDF_ARGS *args,
                                           char *message) {
  if (args->arg_count != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_set_expected_queries takes 1 argument");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

__attribute__((visibility("default")))
char *mysql_accel_set_expected_queries(UDF_INIT *, UDF_ARGS *args, char *result,
                                       unsigned long *length, unsigned char *,
                                       unsigned char *) {
  mysql_rt::MysqlSetExpectedQueries(std::strtod(ArgString(args, 0).c_str(), nullptr));
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

__attribute__((visibility("default")))
bool mysql_accel_get_last_maintenance_action_init(UDF_INIT *initid,
                                                  UDF_ARGS *args,
                                                  char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_get_last_maintenance_action takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 64;
  return false;
}

__attribute__((visibility("default")))
char *mysql_accel_get_last_maintenance_action(UDF_INIT *, UDF_ARGS *,
                                              char *result,
                                              unsigned long *length,
                                              unsigned char *,
                                              unsigned char *) {
  const char *action = mysql_rt::MysqlLastMaintenanceAction();
  std::size_t n = std::strlen(action);
  if (n > 63) {
    n = 63;
  }
  std::memcpy(result, action, n);
  result[n] = '\0';
  *length = static_cast<unsigned long>(n);
  return result;
}

} // extern "C"
