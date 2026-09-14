// TPC-H aux UDFs + observer/maintenance helpers for the MySQL plugin.
#include "accelerator_codegen/mysql/plugin_minimal.h"
#include "mysql/runtime.hpp"

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"

#include <algorithm>
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

extern "C" {

#define TPCH_AUX_UDF(N, TYPES, NARGS)                                          \
  __attribute__((visibility("default"))) bool                                  \
      mysql_accel_tpch_q##N##_aux_init(UDF_INIT *initid, UDF_ARGS *args,       \
                                       char *message) {                        \
    if (InitStringUdf(initid, args, NARGS)) {                                  \
      std::snprintf(message, MYSQL_ERRMSG_SIZE,                                \
                    "mysql_accel_tpch_q" #N "_aux argument count");            \
      return true;                                                             \
    }                                                                          \
    return false;                                                              \
  }                                                                            \
  __attribute__((visibility("default"))) void                                  \
      mysql_accel_tpch_q##N##_aux_deinit(UDF_INIT *initid) {                   \
    DeinitStringUdf(initid);                                                   \
  }                                                                            \
  __attribute__((visibility("default"))) char *mysql_accel_tpch_q##N##_aux(    \
      UDF_INIT *initid, UDF_ARGS *args, char *, unsigned long *length,         \
      unsigned char *, unsigned char *error) {                                 \
    try {                                                                      \
      auto parsed = ParseTypedArgs(args, TYPES);                               \
      return CopyResult(initid, mysql_rt::ExecuteMysqlTpchQ##N(parsed),        \
                        length, error);                                        \
    } catch (...) {                                                            \
      *error = 1;                                                              \
      *length = 0;                                                             \
      return initid->ptr;                                                      \
    }                                                                          \
  }

TPCH_AUX_UDF(1, "i", 1)
TPCH_AUX_UDF(2, "iss", 3)
TPCH_AUX_UDF(3, "sd", 2)
TPCH_AUX_UDF(4, "d", 1)
TPCH_AUX_UDF(5, "sd", 2)
TPCH_AUX_UDF(6, "dff", 3)
TPCH_AUX_UDF(7, "ss", 2)
TPCH_AUX_UDF(8, "sss", 3)
TPCH_AUX_UDF(9, "s", 1)
TPCH_AUX_UDF(10, "d", 1)
TPCH_AUX_UDF(11, "sf", 2)
TPCH_AUX_UDF(12, "ssd", 3)
TPCH_AUX_UDF(13, "s", 1)
TPCH_AUX_UDF(14, "d", 1)
TPCH_AUX_UDF(15, "d", 1)
TPCH_AUX_UDF(16, "ssiiiiiiii", 10)
TPCH_AUX_UDF(17, "ss", 2)
TPCH_AUX_UDF(18, "i", 1)
TPCH_AUX_UDF(19, "sisisi", 6)
TPCH_AUX_UDF(20, "dss", 3)
TPCH_AUX_UDF(21, "s", 1)
TPCH_AUX_UDF(22, "sssssss", 7)

#undef TPCH_AUX_UDF

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
