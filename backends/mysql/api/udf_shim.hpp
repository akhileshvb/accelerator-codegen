// Per-query UDF shims.  The MySQL server resolves a UDF `<name>` by the exported
// symbols `<name>`, `<name>_init` and `<name>_deinit`, so each query needs its
// own symbols; they live in the query's dir (q<N>/mysql/plugin/udf.cpp) and
// forward to the generic implementation in mysql_plugin.cpp, which finds the
// query in the UDF registry (runtime.hpp) by name:
//
//   #include "backends/mysql/api/udf_shim.hpp"
//   ACCEL_MYSQL_QUERY_UDF(mysql_accel_<query>_aux)
#pragma once

#include "backends/mysql/engine/plugin_minimal.h"

namespace accelerator_codegen::aux_state::engine::mysql {

// Generic UDF implementation (mysql_plugin.cpp).  `name` is the registered UDF name.
bool MysqlUdfInit(UDF_INIT *initid, UDF_ARGS *args, char *message, const char *name);
void MysqlUdfDeinit(UDF_INIT *initid);
char *MysqlUdfCall(UDF_INIT *initid, UDF_ARGS *args, unsigned long *length,
                   unsigned char *is_null, unsigned char *error, const char *name);

} // namespace accelerator_codegen::aux_state::engine::mysql

#define ACCEL_MYSQL_QUERY_UDF(NAME)                                            \
  extern "C" {                                                                 \
  __attribute__((visibility("default"))) bool NAME##_init(                     \
      UDF_INIT *initid, UDF_ARGS *args, char *message) {                       \
    return ::accelerator_codegen::aux_state::engine::mysql::MysqlUdfInit(      \
        initid, args, message, #NAME);                                         \
  }                                                                            \
  __attribute__((visibility("default"))) void NAME##_deinit(UDF_INIT *initid) { \
    ::accelerator_codegen::aux_state::engine::mysql::MysqlUdfDeinit(initid);   \
  }                                                                            \
  __attribute__((visibility("default"))) char *NAME(                           \
      UDF_INIT *initid, UDF_ARGS *args, char *, unsigned long *length,         \
      unsigned char *is_null, unsigned char *error) {                          \
    return ::accelerator_codegen::aux_state::engine::mysql::MysqlUdfCall(      \
        initid, args, length, is_null, error, #NAME);                          \
  }                                                                            \
  }
