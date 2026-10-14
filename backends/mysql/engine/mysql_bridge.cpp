// GENERATED BACKEND — txn bridge layer: MySQL audit plugin + mutation capture UDFs.
// Wires MySQL audit events → the accelerators' per-session transaction
// bookkeeping (DispatchMysqlStatementEnd / DispatchMysqlSessionEnd).
// Capture UDFs (called by the mutation triggers): mysql_accel_capture_row
// (self-describing), mysql_accel_mutations_capture_row and its schema
// registration mysql_accel_mutations_schema.
//
// The audit plugin receives MYSQL_AUDIT_GENERAL_STATUS events when a
// statement completes. COMMIT / ROLLBACK arrive as general-status events
// with the query text; this wires them to the coordinator the way a
// Postgres RegisterXactCallback would.
//
// Gate-test UDFs (toy txn bridge, mutation inspection, scan and runtime
// drivers) live in backends/mysql/tests/mysql_bridge_test_udfs.cpp and are
// only built into the test plugin variant; they observe this bridge through
// MysqlBridgeObserver (mysql_bridge.hpp).

#include "backends/mysql/engine/plugin_minimal.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

// ---- Accelerator substrate ----
#include "substrate/aux/core/aux_types.hpp"
#include "backends/mysql/engine/mysql_bridge.hpp"
#include "backends/mysql/engine/mysql_mutation_capture.hpp"
#include "backends/mysql/engine/mysql_xact_callbacks.hpp"

namespace accel = accelerator_codegen::aux_state;
namespace core = accel::core;
namespace engine = accel::engine::mysql;

namespace {

// Bridge observer (none in the production plugin; see mysql_bridge.hpp).
std::atomic<engine::MysqlBridgeObserver *> g_observer{nullptr};

// ---- Audit notification handler ----
// Receives general-status events after COMMIT / ROLLBACK complete.

static int AuditNotify(MYSQL_THD thd, mysql_event_class_t event_class,
                       const void *event_raw) {
  if (event_class == MYSQL_AUDIT_CONNECTION_CLASS) {
    // A closed connection rolls its open transaction back (no ROLLBACK event).
    auto *ev = static_cast<const mysql_event_connection *>(event_raw);
    if (ev->event_subclass == MYSQL_AUDIT_CONNECTION_DISCONNECT) {
      try {
        engine::DispatchMysqlSessionEnd(engine::MysqlSessionFromThd(thd));
      } catch (...) {
      }
    }
    return 0;
  }
  if (event_class != MYSQL_AUDIT_GENERAL_CLASS) return 0;
  auto *ev = static_cast<const mysql_event_general *>(event_raw);
  if (ev->event_subclass != MYSQL_AUDIT_GENERAL_STATUS) return 0;

  // The statement's SQL command name as the server spells it ("commit",
  // "rollback", "rollback_to_savepoint", "begin", "update", ...).
  auto is_command = [&](const char *name) -> bool {
    const char *str = ev->general_sql_command.str;
    const size_t len = ev->general_sql_command.length;
    const size_t nlen = strlen(name);
    if (!str || len != nlen) return false;
    for (size_t i = 0; i < nlen; ++i) {
      char c = str[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
      if (c != name[i]) return false;
    }
    return true;
  };

  const bool is_commit = is_command("commit");
  const bool is_rollback = is_command("rollback");

  try {
    engine::MysqlStatementKind kind = engine::MysqlStatementKind::Other;
    if (is_commit) kind = engine::MysqlStatementKind::Commit;
    else if (is_rollback) kind = engine::MysqlStatementKind::Rollback;
    else if (is_command("rollback_to_savepoint")) kind = engine::MysqlStatementKind::RollbackToSavepoint;
    else if (is_command("begin")) kind = engine::MysqlStatementKind::Begin;
    // A failed statement was rolled back: drop what its triggers captured.
    engine::DispatchMysqlStatementEnd(engine::MysqlSessionFromThd(thd), kind,
                                      /*failed=*/ev->general_error_code != 0,
                                      engine::MysqlThdInActiveTransaction(thd));
    if (is_commit || is_rollback) {
      if (auto *observer = g_observer.load()) {
        if (is_commit) observer->OnCommit();
        else observer->OnRollback();
      }
    }
  } catch (...) {
    // Must not surface C++ exceptions through the MySQL audit handler.
  }
  return 0;
}

// ---- Mutation capture UDFs (trigger-called) ----

static bool UdfInitMutationsCaptureRow(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  // Dynamic arg count: table_id, kind, then old-row columns, then new-row columns.
  // Minimum: table_id + kind (2). Maximum: large enough for both old+new rows.
  if (args->arg_count < 2 || args->arg_count > 256) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_mutations_capture_row takes 2-256 arguments");
    return true;
  }
  if (args->arg_type) {
    for (unsigned i = 0; i < args->arg_count; ++i) {
      args->arg_type[i] = STRING_RESULT;
    }
  }
  initid->maybe_null = 0;
  initid->max_length = 64;
  return false;
}

static char *CaptureRow(UDF_ARGS *args, char *result, unsigned long *length,
                        bool typed) {
  if (!args || args->arg_count < 2) {
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }

  try {
    // Build typed MutationBatch from trigger arguments (no coordinator needed for capture)
    auto batch_opt = typed ? engine::BuildMutationBatchFromTypedTriggerArgs(
                                 args->arg_count, args->args, args->lengths)
                           : engine::BuildMutationBatchFromTriggerArgs(
                                 args->arg_count, args->args, args->lengths);

    if (!batch_opt.has_value()) {
      // The row could not be decoded: the accelerators may be missing it.
      engine::InvalidateAllMysqlAccelerators();
    } else {
      core::MutationBatch &batch = *batch_opt;
      engine::DispatchMysqlMutation(batch);
      if (auto *observer = g_observer.load()) {
        observer->OnCapturedMutation(batch);
      }
    }

    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    // Never fail the user's write because of the accelerator; but the
    // accelerators missed it, so none of their states may be trusted.
    try { engine::InvalidateAllMysqlAccelerators(); } catch (...) {}
    std::strcpy(result, "ok");
    *length = 2;
    return result;
  }
}

// ---- Mutation schema registration UDF ----

static bool UdfInitMutationsSchema(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  // args: table_id, col0_type, col0_nullable, col1_type, col1_nullable, ...
  if (args->arg_count < 3 || args->arg_count % 2 != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_mutations_schema takes table_id + N*(type,nullable) pairs");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfMutationsSchema(UDF_INIT *, UDF_ARGS *args,
                                 char *result, unsigned long *length,
                                 unsigned char *, unsigned char *) {
  if (!args || args->arg_count < 3) {
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }

  // Read an integer arg value. MySQL can pass integer literals as:
  // (a) char* string when lengths[idx] > 0, or
  // (b) long long* binary when lengths[idx] == 0 (args[idx] points to a long long).
  auto read_int = [&](unsigned int idx, long long default_val) -> long long {
    if (!args->args[idx]) return default_val;
    if (args->lengths && args->lengths[idx] > 0) {
      std::string s(args->args[idx], args->lengths[idx]);
      return std::strtoll(s.c_str(), nullptr, 10);
    }
    return *reinterpret_cast<long long *>(args->args[idx]);
  };

  core::TableId table_id = static_cast<core::TableId>(read_int(0, 0));

  std::vector<engine::CaptureColumn> cols;
  for (unsigned int i = 1; i + 1 < args->arg_count; i += 2) {
    engine::CaptureColumn col;
    int type_code = static_cast<int>(read_int(i, 6));
    switch (type_code) {
      case 0: col.type = core::AuxType::Boolean; break;
      case 1: col.type = core::AuxType::Int64; break;
      case 2: col.type = core::AuxType::UInt64; break;
      case 3: col.type = core::AuxType::ScaledDecimal; break;
      case 4: col.type = core::AuxType::Double; break;
      case 5: col.type = core::AuxType::Date; break;
      default: col.type = core::AuxType::String; break;
    }
    col.nullable = (read_int(i + 1, 0) != 0);
    cols.push_back(col);
  }

  engine::RegisterCaptureTableSchema(table_id, std::move(cols));

  std::strcpy(result, "ok");
  *length = 2;
  return result;
}


// ---- Plugin descriptors ----

static struct st_mysql_audit g_audit_descriptor = {
  MYSQL_AUDIT_INTERFACE_VERSION,
  nullptr,  /* release_thd */
  AuditNotify,
  { [MYSQL_AUDIT_GENERAL_CLASS] = MYSQL_AUDIT_GENERAL_STATUS,
    [MYSQL_AUDIT_CONNECTION_CLASS] = MYSQL_AUDIT_CONNECTION_DISCONNECT },
};

static int mysql_plugin_init(void *) {
  return 0;
}

static int mysql_plugin_deinit(void *) {
  if (auto *observer = g_observer.load()) {
    observer->OnPluginDeinit();
  }
  return 0;
}

} // anonymous namespace

namespace accelerator_codegen::aux_state::engine::mysql {

void SetMysqlBridgeObserver(MysqlBridgeObserver *observer) {
  g_observer.store(observer);
}

} // namespace accelerator_codegen::aux_state::engine::mysql

// ---- Plugin declaration ----

extern "C" {
mysql_declare_plugin(accelerator_codegen_txn) {
  MYSQL_AUDIT_PLUGIN,
  &g_audit_descriptor,
  "accelerator_codegen_audit",
  "Codex Pipeline",
  "Accelerator codegen txn bridge audit plugin",
  PLUGIN_LICENSE_GPL,
  mysql_plugin_init,
  nullptr,           /* check_uninstall */
  mysql_plugin_deinit,
  0x0100,            /* version */
  nullptr,           /* status_vars */
  nullptr,           /* system_vars */
  nullptr,
  0,
}
mysql_declare_plugin_end;

// ---- Mutations UDF exports ----

__attribute__((visibility("default")))
bool mysql_accel_mutations_capture_row_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsCaptureRow(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_capture_row(UDF_INIT *, UDF_ARGS *args,
                                         char *result, unsigned long *length,
                                         unsigned char *, unsigned char *) {
  return CaptureRow(args, result, length, /*typed=*/false);
}

// Self-describing capture (args: table_id, kind, type string, old row, new
// row; see BuildMutationBatchFromTypedTriggerArgs).
__attribute__((visibility("default")))
bool mysql_accel_capture_row_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count < 3) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_capture_row takes table_id, kind, types, columns...");
    return true;
  }
  return UdfInitMutationsCaptureRow(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_capture_row(UDF_INIT *, UDF_ARGS *args, char *result,
                              unsigned long *length, unsigned char *,
                              unsigned char *) {
  return CaptureRow(args, result, length, /*typed=*/true);
}

__attribute__((visibility("default")))
bool mysql_accel_mutations_schema_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsSchema(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_schema(UDF_INIT *initid, UDF_ARGS *args,
                                    char *result, unsigned long *length,
                                    unsigned char *is_null, unsigned char *error) {
  return UdfMutationsSchema(initid, args, result, length, is_null, error);
}


} // extern "C"
