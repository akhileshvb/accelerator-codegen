// GENERATED BACKEND — txn bridge layer: MySQL audit plugin + mutation capture UDF.
// Wires MySQL audit events → MysqlHost's per-session transaction bookkeeping
// (OnStatementEnd / OnSessionEnd), and the capture UDF the mutation triggers
// call (mysql_accel_capture_row, self-describing) → MysqlHost::OnCapturedRow.
//
// The audit plugin receives MYSQL_AUDIT_GENERAL_STATUS events when a
// statement completes. COMMIT / ROLLBACK arrive as general-status events
// with the query text; this wires them to the host the way a Postgres
// RegisterXactCallback would.
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
#include "substrate/aux/runtime/accelerator_runtime.hpp"
#include "backends/mysql/api/mysql_host.hpp"
#include "backends/mysql/engine/mysql_bridge.hpp"
#include "backends/mysql/engine/mysql_session.hpp"

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
        engine::MysqlHost::Get().OnSessionEnd(engine::MysqlSessionFromThd(thd));
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
    engine::MysqlHost::Get().OnStatementEnd(engine::MysqlSessionFromThd(thd), kind,
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

// ---- Mutation capture UDF (trigger-called) ----

static bool UdfInitCaptureRow(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  // table id, kind, column types, then the old row's columns and the new row's.
  if (args->arg_count < 3 || args->arg_count > 256) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_capture_row takes table_id, kind, types, columns...");
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

// Never fails the user's write because of the accelerator; a row that cannot
// be captured invalidates every accelerator's state instead.
static char *CaptureRow(UDF_ARGS *args, char *result, unsigned long *length) {
  const auto session = engine::CurrentMysqlSession();
  try {
    const auto row = engine::DecodeCapturedRow(args->arg_count, args->args, args->lengths);
    engine::MysqlHost::Get().OnCapturedRow(session, row);
    if (auto *observer = g_observer.load()) {
      observer->OnCapturedRow(row);
    }
  } catch (...) {
    try { engine::MysqlHost::Get().OnCaptureFailure(session); } catch (...) {}
  }
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

// ---- Thread settings (0: the default, see runtime/thread_budget.hpp) ----

enum Setting { kBudget, kRead, kBuild, kMaintenance, kSettingCount };
unsigned long g_settings[kSettingCount] = {};

void ApplyThreadSettings() {
  engine::MysqlHost::Get().SetThreads(
      {g_settings[kBudget], g_settings[kRead], g_settings[kBuild], g_settings[kMaintenance]});
}

void UpdateThreadSetting(MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  *static_cast<unsigned long *>(var_ptr) = *static_cast<const unsigned long *>(save);
  ApplyThreadSettings();
}

// The server names each variable <plugin name>_<name>.
st_mysql_sysvar_ulong ThreadSysvar(const char *name, const char *comment, Setting setting) {
  return {PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED, name, comment, nullptr, UpdateThreadSetting,
          &g_settings[setting], /*def*/ 0, /*min*/ 0, /*max*/ 4096, /*blk*/ 0};
}

// accelerator_codegen_audit_accel_force_fallback: every accelerated read answers from SQL
// (differential tests).
unsigned long g_force_fallback = 0;

void UpdateForceFallback(MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  g_force_fallback = *static_cast<const unsigned long *>(save);
  *static_cast<unsigned long *>(var_ptr) = g_force_fallback;
  accel::runtime::SetForcedFallback(g_force_fallback != 0);
}

st_mysql_sysvar_ulong g_force_fallback_sysvar = {
    PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED, "accel_force_fallback",
    "Answer every accelerated query from SQL (differential tests)", nullptr, UpdateForceFallback,
    &g_force_fallback, /*def*/ 0, /*min*/ 0, /*max*/ 1, /*blk*/ 0};

st_mysql_sysvar_ulong g_sysvars[kSettingCount] = {
    ThreadSysvar("accel_threads", "Accelerator thread budget (0: the CPUs this server may use)", kBudget),
    ThreadSysvar("accel_read_threads", "Threads one accelerated read may use (0: the budget)", kRead),
    ThreadSysvar("accel_build_threads", "Threads one state build may use (0: the budget)", kBuild),
    ThreadSysvar("accel_maint_threads", "Threads one commit's maintenance may use (0: serial)",
                 kMaintenance),
};

SYS_VAR *g_system_vars[kSettingCount + 2] = {
    reinterpret_cast<SYS_VAR *>(&g_sysvars[kBudget]), reinterpret_cast<SYS_VAR *>(&g_sysvars[kRead]),
    reinterpret_cast<SYS_VAR *>(&g_sysvars[kBuild]),
    reinterpret_cast<SYS_VAR *>(&g_sysvars[kMaintenance]),
    reinterpret_cast<SYS_VAR *>(&g_force_fallback_sysvar), nullptr};

static int mysql_plugin_init(void *) {
  ApplyThreadSettings();
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
  g_system_vars,
  nullptr,
  0,
}
mysql_declare_plugin_end;

// ---- Capture UDF export ----

__attribute__((visibility("default")))
bool mysql_accel_capture_row_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitCaptureRow(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_capture_row(UDF_INIT *, UDF_ARGS *args, char *result,
                              unsigned long *length, unsigned char *,
                              unsigned char *) {
  return CaptureRow(args, result, length);
}

} // extern "C"
