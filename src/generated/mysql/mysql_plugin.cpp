// Generic query-UDF implementation + observer/maintenance UDFs for the MySQL plugin.
// Query UDF symbols live in each query's dir (see udf_shim.hpp).
#include "accelerator_codegen/mysql/plugin_minimal.h"
#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"

#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace core = accelerator_codegen::aux_state::core;
namespace mysql_rt = accelerator_codegen::aux_state::engine::mysql;

namespace {

constexpr unsigned long kMaxResult = 8u * 1024u * 1024u;

int32_t ParseUnixDays(const std::string &text) {
  if (text.size() < 10) {
    return 0;
  }
  int y = std::atoi(text.substr(0, 4).c_str());
  unsigned m = static_cast<unsigned>(std::atoi(text.substr(5, 2).c_str()));
  unsigned d = static_cast<unsigned>(std::atoi(text.substr(8, 2).c_str()));
  if (y == 0 || m == 0 || d == 0) {
    return 0;
  }
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

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

core::QueryArguments ParseTypedArgs(UDF_ARGS *args, const char *types) {
  core::QueryArguments out;
  unsigned i = 0;
  for (const char *t = types; *t; ++t, ++i) {
    const std::string text = ArgString(args, i);
    switch (*t) {
    case 'i':
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Int64, static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10))));
      break;
    case 'f':
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Double, std::strtod(text.c_str(), nullptr)));
      break;
    case 'd':
      out.values.push_back(core::AuxValue::Of(
          core::AuxType::Date, core::AuxDate{ParseUnixDays(text)}));
      break;
    default:
      out.values.push_back(core::AuxValue::Of(core::AuxType::String, text));
      break;
    }
  }
  return out;
}

bool InitStringUdf(UDF_INIT *initid, UDF_ARGS *args, unsigned expected) {
  if (args->arg_count != expected) {
    return true;
  }
  if (args->arg_type) {
    for (unsigned i = 0; i < args->arg_count; ++i) {
      args->arg_type[i] = STRING_RESULT;
    }
  }
  initid->maybe_null = 0;
  initid->max_length = kMaxResult;
  initid->ptr = static_cast<char *>(std::malloc(kMaxResult));
  return initid->ptr == nullptr;
}

void DeinitStringUdf(UDF_INIT *initid) {
  if (initid && initid->ptr) {
    std::free(initid->ptr);
    initid->ptr = nullptr;
  }
}

char *CopyResult(UDF_INIT *initid, const std::string &text, unsigned long *length,
                 unsigned char *error) {
  if (!initid || !initid->ptr) {
    *error = 1;
    *length = 0;
    return nullptr;
  }
  const std::size_t n = std::min(text.size(), static_cast<std::size_t>(kMaxResult - 1));
  std::memcpy(initid->ptr, text.data(), n);
  initid->ptr[n] = '\0';
  *length = static_cast<unsigned long>(n);
  return initid->ptr;
}

} // namespace

namespace accelerator_codegen::aux_state::engine::mysql {

// Generic query UDF implementation; the per-query extern "C" symbols
// (q<N>/mysql/plugin/udf.cpp, ACCEL_MYSQL_QUERY_UDF) call these.
bool MysqlUdfInit(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *name) {
  const MysqlUdfEntry *entry = FindMysqlUdf(name);
  if (!entry || InitStringUdf(initid, args, static_cast<unsigned>(entry->arg_types.size()))) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE, "%s argument count", name);
    return true;
  }
  return false;
}

void MysqlUdfDeinit(UDF_INIT *initid) { DeinitStringUdf(initid); }

char *MysqlUdfCall(UDF_INIT *initid, UDF_ARGS *args, unsigned long *length,
                   unsigned char *error, const char *name) {
  try {
    const MysqlUdfEntry *entry = FindMysqlUdf(name);
    if (!entry) {
      throw std::runtime_error("no MySQL UDF registered");
    }
    if (entry->null_first_arg_yields_empty &&
        (args->arg_count < 1 || args->args[0] == nullptr)) {
      return CopyResult(initid, std::string(), length, error);
    }
    auto parsed = ParseTypedArgs(args, entry->arg_types.c_str());
    return CopyResult(initid, entry->execute(parsed), length, error);
  } catch (...) {
    *error = 1;
    *length = 0;
    return initid->ptr;
  }
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
