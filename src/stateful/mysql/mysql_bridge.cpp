// GENERATED BACKEND — txn bridge layer: MySQL audit plugin + UDF registration.
// Wires MySQL audit events → coordinator OnPreCommit/OnCommit/OnAbort.
// UDFs: mysql_accel_txn_init, mysql_accel_txn_capture, mysql_accel_txn_read,
//       mysql_accel_txn_epoch, mysql_accel_txn_reset.
// Mutation UDFs: mysql_accel_mutations_capture_row, mysql_accel_mutations_last,
//       mysql_accel_mutations_drain, mysql_accel_mutations_count.
//
// The audit plugin receives MYSQL_AUDIT_GENERAL_STATUS events when a
// statement completes. COMMIT / ROLLBACK arrive as general-status events
// with the query text; this wires them to the coordinator the way a
// Postgres RegisterXactCallback would.

#include "accelerator_codegen/mysql/plugin_minimal.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// ---- Accelerator substrate ----
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_state_store.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_mutation_capture.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"
#include "accelerator_codegen/runtime/mysql/scan_helper.hpp"

namespace accel = accelerator_codegen::aux_state;
namespace core = accel::core;
namespace engine = accel::engine::mysql;

namespace {

struct ToyPayload {
  int count;
  std::string label;
};

class TxnToyModule final : public core::AuxQueryModule {
public:
  core::AcceleratorId Id() const override { return "txn_toy"; }

  std::vector<core::TableDependency> Dependencies() const override {
    core::TableDependency dep;
    dep.table = 1;
    return {dep};
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<core::BasicAuxTxnBuffer>();
  }

  void Build(core::AuxEngineServices &, core::StateBuilder &builder) override {
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{1, "built"});
    builder.Write(core::StateImage::Make(Id(), 1, 1, std::move(payload)));
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override {
    core::InvalidationPlan plan;
    plan.scope = accel::InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override {}

  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &,
                   const core::InvalidationPlan &,
                   core::AuxTxnBuffer &) override {
    core::StagedMaintenanceOutcome outcome;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    auto payload = std::make_shared<const ToyPayload>(ToyPayload{42, "staged"});
    outcome.updates.push_back(core::StateImage::Make(Id(), 1, 1, std::move(payload)));
    return outcome;
  }

  bool CheckValidity(core::AuxEngineServices &,
                     const core::QueryArguments &) const override {
    return true;
  }

  void Execute(core::AuxEngineServices &,
               const core::QueryArguments &,
               core::AuxRowSink &) override {}

  void ExecuteFallback(core::AuxEngineServices &,
                       const core::QueryArguments &,
                       core::AuxRowSink &sink) override {
    (void)sink;
  }
};

// ---- Mutation capture state ----
// Typed MutationBatch storage for harness inspection UDFs.
// The primary capture path flows through g_backend->CaptureMutation() →
// coordinator.OnMutation() for InterpretMutation/AccumulateMutation/StageMaintenance.

std::vector<core::MutationBatch> g_captured_mutation_batches;
std::mutex g_mutations_mutex;

// ---- Bridge state (shared between audit hook and UDFs) ----

std::unique_ptr<engine::MysqlStateStore> g_state_store;
std::unique_ptr<core::AuxTransactionCoordinator> g_coordinator;
std::unique_ptr<engine::MysqlAuxBackend> g_backend;
std::shared_ptr<core::AuxQueryModule> g_toy_module;
core::Version g_current_epoch = 0;
bool g_initialized = false;
std::mutex g_mutex;

// ---- Audit notification handler ----
// Receives general-status events after COMMIT / ROLLBACK complete.

static int AuditNotify(MYSQL_THD, mysql_event_class_t event_class,
                       const void *event_raw) {
  if (event_class != MYSQL_AUDIT_GENERAL_CLASS) return 0;
  auto *ev = static_cast<const mysql_event_general *>(event_raw);
  if (ev->event_subclass != MYSQL_AUDIT_GENERAL_STATUS) return 0;

  auto has_word = [](const char *str, size_t len, const char *word) -> bool {
    if (!str || len == 0) return false;
    size_t wlen = strlen(word);
    if (len < wlen) return false;
    for (size_t i = 0; i < wlen; ++i) {
      char c = str[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
      if (c != word[i]) return false;
    }
    return true;
  };

  bool is_commit = has_word(ev->general_sql_command.str, ev->general_sql_command.length, "commit");
  bool is_rollback = has_word(ev->general_sql_command.str, ev->general_sql_command.length, "rollback");
  bool is_dml =
      has_word(ev->general_sql_command.str, ev->general_sql_command.length, "update") ||
      has_word(ev->general_sql_command.str, ev->general_sql_command.length, "insert") ||
      has_word(ev->general_sql_command.str, ev->general_sql_command.length, "delete");

  try {
    if (is_commit || is_rollback) {
      engine::DispatchMysqlTxnEvent(is_commit, is_rollback);
    } else if (is_dml) {
      // Autocommit DML: there is no separate COMMIT event.
      engine::DispatchMysqlTxnEvent(true, false);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || !g_backend) return 0;
    if (is_commit) {
      const bool had_work = g_coordinator && g_coordinator->HasPendingWork();
      g_backend->OnPreCommit();
      g_backend->OnCommit();
      if (had_work) {
        ++g_current_epoch;
      }
    } else if (is_rollback) {
      g_backend->OnAbort();
    }
  } catch (...) {
    // Must not surface C++ exceptions through the MySQL audit handler.
  }
  return 0;
}

// ---- UDF helpers ----

static bool UdfInitTxnInit(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_txn_init takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static long long UdfTxnInit(UDF_INIT *, UDF_ARGS *,
                            char *is_null, char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);

    g_toy_module = std::make_shared<TxnToyModule>();

    g_state_store = std::make_unique<engine::MysqlStateStore>(
        []() -> core::Version {
          return g_current_epoch;
        });

    g_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
    g_coordinator->Register(g_toy_module);

    g_backend = std::make_unique<engine::MysqlAuxBackend>(
        *g_coordinator, *g_state_store,
        []() -> core::VisibilityToken {
          core::VisibilityToken tok;
          tok.mutation_epoch = g_current_epoch;
          return tok;
        });

    g_current_epoch = 0;
    g_initialized = true;
    return 1;
  } catch (...) {
    *error = 1;
    return 0;
  }
}

// MySQL integer literals arrive as INT_RESULT (pointer to long long). Asking
// the server to coerce to STRING_RESULT in init is the documented path; the
// readers below still accept both so CaptureMutation gets table_id=1.
static long long ReadUdfIntArg(UDF_ARGS *args, unsigned idx, long long def = 0) {
  if (!args || idx >= args->arg_count || !args->args[idx]) {
    return def;
  }
  if (args->arg_type && args->arg_type[idx] == INT_RESULT) {
    return *reinterpret_cast<long long *>(args->args[idx]);
  }
  if (args->lengths && args->lengths[idx] > 0) {
    std::string s(args->args[idx], args->lengths[idx]);
    return std::strtoll(s.c_str(), nullptr, 10);
  }
  return *reinterpret_cast<long long *>(args->args[idx]);
}

static std::string ReadUdfStringArg(UDF_ARGS *args, unsigned idx) {
  if (!args || idx >= args->arg_count || !args->args[idx]) {
    return {};
  }
  if (args->arg_type && args->arg_type[idx] == INT_RESULT) {
    return std::to_string(*reinterpret_cast<long long *>(args->args[idx]));
  }
  if (args->lengths) {
    return std::string(args->args[idx], args->lengths[idx]);
  }
  return std::string(args->args[idx]);
}

static bool UdfInitTxnCapture(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 4) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_txn_capture takes 4 arguments");
    return true;
  }
  if (args->arg_type) {
    for (unsigned i = 0; i < args->arg_count; ++i) {
      args->arg_type[i] = STRING_RESULT;
    }
  }
  initid->maybe_null = 1;
  initid->max_length = 4096;
  return false;
}

static char *UdfTxnCapture(UDF_INIT *, UDF_ARGS *args,
                           char *result, unsigned long *length,
                           unsigned char *is_null, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || !g_backend) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }

    core::MutationBatch batch;
    batch.table = static_cast<core::TableId>(ReadUdfIntArg(args, 0, 0));
    batch.kind = static_cast<core::MutationKind>(ReadUdfIntArg(args, 1, 0));

    if (args->args[2]) {
      core::AuxRow row;
      core::AuxValue val;
      val.type = core::AuxType::Int64;
      val.is_null = false;
      val.payload = static_cast<int64_t>(ReadUdfIntArg(args, 2, 0));
      row.values.push_back(val);
      batch.new_rows.push_back(row);
    }

    std::string label = ReadUdfStringArg(args, 3);
    if (!label.empty()) {
      if (batch.new_rows.empty()) {
        core::AuxRow row;
        batch.new_rows.push_back(row);
      }
      if (label == "NULL") {
        batch.new_rows.back().values.push_back(
            core::AuxValue::Null(core::AuxType::String));
      } else {
        batch.new_rows.back().values.push_back(
            core::AuxValue::Of(core::AuxType::String, label));
      }
    }

    g_backend->CaptureMutation(batch);

    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitTxnRead(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_txn_read takes no arguments");
    return true;
  }
  initid->maybe_null = 1;
  initid->max_length = 4096;
  return false;
}

static char *UdfTxnRead(UDF_INIT *, UDF_ARGS *,
                        char *result, unsigned long *length,
                        unsigned char *is_null, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || !g_state_store) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }

    core::VisibilityToken tok;
    tok.mutation_epoch = g_current_epoch;

    auto snapshot = g_state_store->Read(tok);
    std::ostringstream oss;
    oss << "epoch=" << g_current_epoch
        << " available=" << (snapshot.available ? 1 : 0)
        << " partitions=" << snapshot.partitions.size();
    for (const auto &img : snapshot.partitions) {
      auto *tp = img.As<ToyPayload>();
      if (tp) {
        oss << " count=" << tp->count << " label=" << tp->label
            << " version=" << img.version;
      }
    }

    std::string s = oss.str();
    std::memcpy(result, s.c_str(), s.size() + 1);
    *length = s.size();
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitTxnEpoch(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_txn_epoch takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static char *UdfTxnEpoch(UDF_INIT *, UDF_ARGS *,
                         char *result, unsigned long *length,
                         unsigned char *, unsigned char *) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::ostringstream oss;
  oss << "epoch=" << g_current_epoch;
  std::string s = oss.str();
  std::memcpy(result, s.c_str(), s.size() + 1);
  *length = s.size();
  return result;
}

static bool UdfInitTxnReset(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_txn_reset takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfTxnReset(UDF_INIT *, UDF_ARGS *,
                         char *result, unsigned long *length,
                         unsigned char *, unsigned char *) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_toy_module.reset();
  g_backend.reset();
  g_coordinator.reset();
  g_state_store.reset();
  g_current_epoch = 0;
  g_initialized = false;
  std::strcpy(result, "ok");
  *length = 2;
  return result;
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

static char *UdfMutationsCaptureRow(UDF_INIT *, UDF_ARGS *args,
                                     char *result, unsigned long *length,
                                     unsigned char *, unsigned char *) {
  if (!args || args->arg_count < 2) {
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }

  try {
    // Build typed MutationBatch from trigger arguments (no coordinator needed for capture)
    auto batch_opt = engine::BuildMutationBatchFromTriggerArgs(
        args->arg_count, args->args, args->lengths);

    if (batch_opt.has_value()) {
      core::MutationBatch &batch = *batch_opt;
      engine::DispatchMysqlMutation(nullptr, batch);

      // Feed through g_backend → coordinator pipeline if initialized
      if (g_backend) {
        g_backend->CaptureMutation(batch);
      }

      // Store copy for harness inspection UDFs (last/drain/count)
      {
        std::lock_guard<std::mutex> lock(g_mutations_mutex);
        g_captured_mutation_batches.push_back(std::move(batch));
      }
    }

    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    std::strcpy(result, "ok");
    *length = 2;
    return result;
  }
}

static bool UdfInitMutationsLast(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_mutations_last takes no arguments");
    return true;
  }
  initid->maybe_null = 1;
  initid->max_length = 4096;
  return false;
}

static std::string MutationBatchToString(const core::MutationBatch &batch) {
  std::ostringstream ss;
  ss << "table=" << batch.table << " kind=" << static_cast<int>(batch.kind);
  if (!batch.old_rows.empty()) {
    ss << " old=";
    for (std::size_t i = 0; i < batch.old_rows.size(); ++i) {
      if (i > 0) ss << "|";
      for (std::size_t j = 0; j < batch.old_rows[i].values.size(); ++j) {
        if (j > 0) ss << ";";
        const auto &v = batch.old_rows[i].values[j];
        if (v.is_null) ss << "NULL";
        else if (std::holds_alternative<int64_t>(v.payload)) ss << std::get<int64_t>(v.payload);
        else if (std::holds_alternative<uint64_t>(v.payload)) ss << std::get<uint64_t>(v.payload);
        else if (std::holds_alternative<double>(v.payload)) ss << std::get<double>(v.payload);
        else if (std::holds_alternative<bool>(v.payload)) ss << (std::get<bool>(v.payload) ? "1" : "0");
        else if (std::holds_alternative<std::string>(v.payload)) ss << std::get<std::string>(v.payload);
        else ss << "?";
      }
    }
  }
  if (!batch.new_rows.empty()) {
    ss << " new=";
    for (std::size_t i = 0; i < batch.new_rows.size(); ++i) {
      if (i > 0) ss << "|";
      for (std::size_t j = 0; j < batch.new_rows[i].values.size(); ++j) {
        if (j > 0) ss << ";";
        const auto &v = batch.new_rows[i].values[j];
        if (v.is_null) ss << "NULL";
        else if (std::holds_alternative<int64_t>(v.payload)) ss << std::get<int64_t>(v.payload);
        else if (std::holds_alternative<uint64_t>(v.payload)) ss << std::get<uint64_t>(v.payload);
        else if (std::holds_alternative<double>(v.payload)) ss << std::get<double>(v.payload);
        else if (std::holds_alternative<bool>(v.payload)) ss << (std::get<bool>(v.payload) ? "1" : "0");
        else if (std::holds_alternative<std::string>(v.payload)) ss << std::get<std::string>(v.payload);
        else ss << "?";
      }
    }
  }
  return ss.str();
}

static char *UdfMutationsLast(UDF_INIT *, UDF_ARGS *,
                               char *result, unsigned long *length,
                               unsigned char *is_null, unsigned char *) {
  std::lock_guard<std::mutex> lock(g_mutations_mutex);
  if (g_captured_mutation_batches.empty()) {
    *is_null = 1;
    return nullptr;
  }
  std::string msg = MutationBatchToString(g_captured_mutation_batches.back());
  std::memcpy(result, msg.c_str(), msg.size() + 1);
  *length = msg.size();
  return result;
}

static bool UdfInitMutationsDrain(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_mutations_drain takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfMutationsDrain(UDF_INIT *, UDF_ARGS *,
                                char *result, unsigned long *length,
                                unsigned char *, unsigned char *) {
  {
    std::lock_guard<std::mutex> lock(g_mutations_mutex);
    g_captured_mutation_batches.clear();
  }
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

static bool UdfInitMutationsCount(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_mutations_count takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static char *UdfMutationsCount(UDF_INIT *, UDF_ARGS *,
                                char *result, unsigned long *length,
                                unsigned char *, unsigned char *) {
  std::lock_guard<std::mutex> lock(g_mutations_mutex);
  std::string s = std::to_string(g_captured_mutation_batches.size());
  std::memcpy(result, s.c_str(), s.size() + 1);
  *length = s.size();
  return result;
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


// ========== Real MySQL-native scanner (plugin — reads live MySQL tables) ==========

class RealMysqlCliScanner : public engine::IMysqlTableScanner {
public:
  void Scan(const engine::MysqlScanSpec &spec,
            const engine::MysqlScanRowConsumer &consumer) override {
    // Build a SELECT query that reads from the real MySQL table.
    // This is the engine-native scan path: the plugin opens the table
    // through MySQL's own storage engine via the CLI.
    std::string columns;
    for (std::size_t i = 0; i < spec.projected_columns.size(); ++i) {
      if (i > 0) columns += ", ";
      columns += spec.projected_columns[i].name;
    }
    if (columns.empty()) columns = "*";

    std::string where;
    if (spec.filter.has_value()) {
      const auto &f = *spec.filter;
      if (f.column_index < spec.projected_columns.size()) {
        where = " WHERE " + spec.projected_columns[f.column_index].name +
                " = \"" + f.value + "\"";
      }
    }

    std::string sql = "SELECT " + columns + " FROM " + spec.table_name + where;
    std::string cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                      "--batch --raw -e \"" + sql + "\" 2>/dev/null";

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return;

    char line_buf[65536];
    // Skip header line
    if (!fgets(line_buf, sizeof(line_buf), pipe)) {
      pclose(pipe);
      return;
    }

    while (fgets(line_buf, sizeof(line_buf), pipe)) {
      std::string line(line_buf);
      // Trim trailing newline
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }

      // Parse tab-separated columns
      std::vector<std::string> cols;
      std::size_t pos = 0;
      while (pos < line.size()) {
        std::size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) {
          std::string val = line.substr(pos);
          if (val == "NULL") val = "";
          cols.push_back(val);
          break;
        }
        std::string val = line.substr(pos, tab - pos);
        if (val == "NULL") val = "";
        cols.push_back(val);
        pos = tab + 1;
      }

      // Pad missing columns with empty string
      while (cols.size() < spec.projected_columns.size()) {
        cols.push_back("");
      }

      consumer(cols, cols.size());
    }

    pclose(pipe);
  }
};

// ========== Scan UDFs ==========

// mysql_accel_scan_init(): register the real MySQL CLI scanner and create
// a test table with seeded data.
static bool UdfInitScanInit(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_scan_init takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfScanInit(UDF_INIT *, UDF_ARGS *,
                          char *result, unsigned long *length,
                          unsigned char *, unsigned char *error) {
  try {
    // Register the real MySQL scanner
    engine::SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

    // Create a test table directly in MySQL and seed data
    std::string drop = "DROP TABLE IF EXISTS toy_scan_table";
    std::string drop_cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                           "-e \"" + drop + "\" 2>/dev/null";
    system(drop_cmd.c_str());

    std::string create = "CREATE TABLE toy_scan_table ("
                         "id INT, name VARCHAR(100), score INT)";
    std::string create_cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                             "-e \"" + create + "\" 2>/dev/null";
    system(create_cmd.c_str());

    std::string insert = "INSERT INTO toy_scan_table VALUES "
                         "(1, \"Alice\", 100), "
                         "(2, \"Bob\", NULL), "
                         "(3, \"Charlie\", 300)";
    std::string insert_cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                             "-e \"" + insert + "\" 2>/dev/null";
    system(insert_cmd.c_str());

    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

// mysql_accel_scan_exec(): call ScanMysqlTableForRebuild against the real
// MySQL table and return the count of rows.
static bool UdfInitScanExec(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_scan_exec takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static char *UdfScanExec(UDF_INIT *, UDF_ARGS *,
                          char *result, unsigned long *length,
                          unsigned char *, unsigned char *error) {
  try {
    engine::MysqlScanSpec spec;
    spec.table_name = "toy_scan_table";
    spec.projected_columns = {{"id"}, {"name"}, {"score"}};

    std::size_t row_count = 0;
    std::string collected;

    engine::ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t n) {
          collected += "R" + std::to_string(row_count) + ":" +
                       "cols=" + std::to_string(n) + "|";
          row_count++;
        });

    std::string s = std::to_string(row_count) + " " + collected;
    std::memcpy(result, s.c_str(), std::min(s.size(), 4095UL));
    result[std::min(s.size(), 4095UL)] = '\0';
    *length = std::min(s.size(), 4095UL);
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

// mysql_accel_scan_proj(): call ScanMysqlTableForRebuild with subset projection.
static bool UdfInitScanProj(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_scan_proj takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static char *UdfScanProj(UDF_INIT *, UDF_ARGS *,
                          char *result, unsigned long *length,
                          unsigned char *, unsigned char *error) {
  try {
    engine::MysqlScanSpec spec;
    spec.table_name = "toy_scan_table";
    spec.projected_columns = {{"name"}, {"score"}};

    std::size_t row_count = 0;
    engine::ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &, std::size_t n) {
          row_count++;
        });

    std::string s = std::to_string(row_count);
    std::memcpy(result, s.c_str(), s.size() + 1);
    *length = s.size();
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

// mysql_accel_scan_filter(): call ScanMysqlTableForRebuild with filter.
static bool UdfInitScanFilter(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_scan_filter takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 32;
  return false;
}

static char *UdfScanFilter(UDF_INIT *, UDF_ARGS *,
                            char *result, unsigned long *length,
                            unsigned char *, unsigned char *error) {
  try {
    engine::MysqlScanSpec spec;
    spec.table_name = "toy_scan_table";
    spec.projected_columns = {{"id"}, {"name"}, {"score"}};
    spec.filter = engine::MysqlScanFilter{1, "Alice"};

    std::size_t row_count = 0;
    engine::ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &, std::size_t) {
          row_count++;
        });

    std::string s = std::to_string(row_count);
    std::memcpy(result, s.c_str(), s.size() + 1);
    *length = s.size();
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

// mysql_accel_scan_clear(): clear test tables.
static bool UdfInitScanClear(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_scan_clear takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfScanClear(UDF_INIT *, UDF_ARGS *,
                           char *result, unsigned long *length,
                           unsigned char *, unsigned char *) {
  try {
    std::string drop = "DROP TABLE IF EXISTS toy_scan_table";
    std::string cmd = "/usr/bin/mysql -h 127.0.0.1 -P 3306 -uroot -pmysql -D accel_mysql_test "
                      "-e \"" + drop + "\" 2>/dev/null";
    system(cmd.c_str());
    engine::SetMysqlTableScanner(nullptr);  // reset to default mock
    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}


// ========== Runtime UDFs (host layer) ==========

struct RuntimeRow {
  std::vector<std::string> cols;
};

class RuntimeToyModule final : public core::AuxQueryModule {
public:
  std::string table_name;
  std::vector<std::string> columns;

  core::AcceleratorId Id() const override { return "runtime_toy"; }

  std::vector<core::TableDependency> Dependencies() const override {
    core::TableDependency dep;
    dep.table = 1;
    dep.mutation_attnums = {};
    return {dep};
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<core::BasicAuxTxnBuffer>();
  }

  void Build(core::AuxEngineServices &svc, core::StateBuilder &builder) override {
    (void)svc;
    auto state = std::make_shared<std::vector<RuntimeRow>>();

    engine::MysqlScanSpec spec;
    spec.table_name = table_name;
    for (const auto &col : columns) {
      spec.projected_columns.push_back({col});
    }

    engine::ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t) {
          state->push_back({cols});
        });

    std::shared_ptr<const std::vector<RuntimeRow>> cst = std::move(state);
    builder.Write(core::StateImage::Make(Id(), 1, 1, cst));
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override {
    core::InvalidationPlan plan;
    plan.scope = accel::InvalidationScope::PartitionSet;
    plan.partitions.insert(1);
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &,
                          core::AuxEngineServices &,
                          core::AuxTxnBuffer &) override {}

  core::StagedMaintenanceOutcome
  StageMaintenance(core::AuxEngineServices &svc,
                   const core::InvalidationPlan &plan,
                   core::AuxTxnBuffer &) override {
    (void)svc;
    core::StagedMaintenanceOutcome outcome;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.invalidations = plan;

    auto state = std::make_shared<std::vector<RuntimeRow>>();
    engine::MysqlScanSpec spec;
    spec.table_name = table_name;
    for (const auto &col : columns) {
      spec.projected_columns.push_back({col});
    }
    engine::ScanMysqlTableForRebuild(spec,
        [&](const std::vector<std::string> &cols, std::size_t) {
          state->push_back({cols});
        });

    std::shared_ptr<const std::vector<RuntimeRow>> cst = std::move(state);
    outcome.updates.push_back(core::StateImage::Make(Id(), 1, 1, cst));
    return outcome;
  }

  bool CheckValidity(core::AuxEngineServices &svc,
                     const core::QueryArguments &) const override {
    auto visibility = svc.CurrentVisibility();
    auto &store = svc.StateStore();
    core::QueryKey key;
    key.accelerator = Id();
    key.value = "1";
    auto result = store.CheckValidity(key, visibility);
    return result.IsValid();
  }

  void Execute(core::AuxEngineServices &svc, const core::QueryArguments &,
               core::AuxRowSink &sink) override {
    auto visibility = svc.CurrentVisibility();
    auto snapshot = svc.StateStore().Read(visibility);

    core::OutputSchema schema;
    for (const auto &col : columns) {
      schema.columns.push_back({col, core::AuxType::String, true});
    }
    sink.Begin(schema);

    for (const auto &image : snapshot.partitions) {
      auto *state = image.As<std::vector<RuntimeRow>>();
      if (!state) continue;
      for (const auto &row : *state) {
        core::AuxRow aux_row;
        for (const auto &val : row.cols) {
          aux_row.values.push_back(core::AuxValue::Of(core::AuxType::String, val));
        }
        sink.WriteRow(std::move(aux_row));
      }
    }
    sink.End();
  }

  void ExecuteFallback(core::AuxEngineServices &,
                       const core::QueryArguments &,
                       core::AuxRowSink &sink) override {
    (void)sink;
  }
};

// ---- Runtime state (separate from txn bridge state) ----

std::unique_ptr<engine::MysqlStateStore> g_rt_state_store;
std::unique_ptr<core::AuxTransactionCoordinator> g_rt_coordinator;
std::unique_ptr<engine::MysqlAuxBackend> g_rt_backend;
std::shared_ptr<core::AuxQueryModule> g_rt_module;
core::Version g_rt_current_epoch = 0;
bool g_rt_initialized = false;
std::mutex g_rt_mutex;

// -------- Runtime UDF init helpers --------

static bool UdfInitRtInit(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 1) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_runtime_init takes 1 argument (table_name)");
    return true;
  }
  if (args->arg_type) {
    args->arg_type[0] = STRING_RESULT;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfRtInit(UDF_INIT *, UDF_ARGS *args,
                        char *result, unsigned long *length,
                        unsigned char *, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_rt_mutex);

    std::string table_name = ReadUdfStringArg(args, 0);

    // Register the real MySQL CLI scanner for host-layer scans.
    engine::SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

    auto mod = std::make_shared<RuntimeToyModule>();
    mod->table_name = table_name;
    mod->columns = {"id", "name", "val"};
    g_rt_module = mod;

    g_rt_state_store = std::make_unique<engine::MysqlStateStore>(
        []() -> core::Version { return g_rt_current_epoch; });

    g_rt_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
    g_rt_coordinator->Register(g_rt_module);

    g_rt_backend = std::make_unique<engine::MysqlAuxBackend>(
        *g_rt_coordinator, *g_rt_state_store,
        []() -> core::VisibilityToken {
          core::VisibilityToken tok;
          tok.mutation_epoch = g_rt_current_epoch;
          return tok;
        });

    g_rt_current_epoch = 0;
    g_rt_initialized = true;

    std::strcpy(result, "ok");
    *length = 2;
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitRtBuild(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_runtime_build takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 64;
  return false;
}

static char *UdfRtBuild(UDF_INIT *, UDF_ARGS *,
                         char *result, unsigned long *length,
                         unsigned char *, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    if (!g_rt_initialized || !g_rt_backend || !g_rt_module || !g_rt_coordinator) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }

    // Inject a synthetic mutation so the coordinator invokes
    // InterpretMutation → StageMaintenance → Commit publish.
    core::MutationBatch seed_mutation;
    seed_mutation.table = 1;
    seed_mutation.kind = core::MutationKind::Insert;
    seed_mutation.new_rows.push_back(core::AuxRow{});
    g_rt_coordinator->OnMutation(seed_mutation);
    g_rt_coordinator->PreCommit();
    g_rt_coordinator->Commit();
    ++g_rt_current_epoch;

    // Re-create coordinator and backend for fresh state (rebuild pattern)
    g_rt_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
    g_rt_coordinator->Register(g_rt_module);

    g_rt_backend = std::make_unique<engine::MysqlAuxBackend>(
        *g_rt_coordinator, *g_rt_state_store,
        []() -> core::VisibilityToken {
          core::VisibilityToken tok;
          tok.mutation_epoch = g_rt_current_epoch;
          return tok;
        });

    std::ostringstream oss;
    oss << "build=ok epoch=" << g_rt_current_epoch;
    std::string s = oss.str();
    std::memcpy(result, s.c_str(), s.size() + 1);
    *length = s.size();
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitRtExecute(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_runtime_execute takes no arguments");
    return true;
  }
  initid->maybe_null = 1;
  initid->max_length = 65535;
  return false;
}

// String-collecting sink for MySQL UDF path (no SETOF return available).
class StringCollectorSink final : public core::AuxRowSink {
public:
  std::string result;

  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override {
    if (!result.empty()) result += "\n";
    for (std::size_t i = 0; i < row.values.size(); ++i) {
      if (i > 0) result += "\t";
      if (row.values[i].is_null) {
        result += "NULL";
      } else {
        result += std::get<std::string>(row.values[i].payload);
      }
    }
  }
  void End() override {}
};

static char *UdfRtExecute(UDF_INIT *, UDF_ARGS *,
                           char *result, unsigned long *length,
                           unsigned char *is_null, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    if (!g_rt_initialized || !g_rt_backend || !g_rt_module) {
      *is_null = 1;
      return result;
    }

    StringCollectorSink sink;
    core::QueryArguments args;
    g_rt_module->Execute(*g_rt_backend, args, sink);

    std::string s = sink.result;
    std::memcpy(result, s.c_str(), std::min(s.size(), 65535UL));
    result[std::min(s.size(), 65535UL)] = '\0';
    *length = std::min(s.size(), 65535UL);
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitRtRead(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_runtime_read takes no arguments");
    return true;
  }
  initid->maybe_null = 1;
  initid->max_length = 4096;
  return false;
}

static char *UdfRtRead(UDF_INIT *, UDF_ARGS *,
                        char *result, unsigned long *length,
                        unsigned char *is_null, unsigned char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_rt_mutex);
    if (!g_rt_initialized || !g_rt_state_store) {
      *is_null = 1;
      return result;
    }

    core::VisibilityToken tok;
    tok.mutation_epoch = g_rt_current_epoch;

    auto snapshot = g_rt_state_store->Read(tok);
    std::ostringstream oss;
    oss << "epoch=" << g_rt_current_epoch
        << " available=" << (snapshot.available ? 1 : 0)
        << " partitions=" << snapshot.partitions.size();

    std::string s = oss.str();
    std::memcpy(result, s.c_str(), s.size() + 1);
    *length = s.size();
    return result;
  } catch (...) {
    *error = 1;
    std::strcpy(result, "error");
    *length = 5;
    return result;
  }
}

static bool UdfInitRtReset(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  if (args->arg_count != 0) {
    std::snprintf(message, MYSQL_ERRMSG_SIZE,
                  "mysql_accel_runtime_reset takes no arguments");
    return true;
  }
  initid->maybe_null = 0;
  initid->max_length = 8;
  return false;
}

static char *UdfRtReset(UDF_INIT *, UDF_ARGS *,
                         char *result, unsigned long *length,
                         unsigned char *, unsigned char *) {
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  g_rt_module.reset();
  g_rt_backend.reset();
  g_rt_coordinator.reset();
  g_rt_state_store.reset();
  g_rt_current_epoch = 0;
  g_rt_initialized = false;
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}


// ---- Plugin descriptors ----

static struct st_mysql_audit g_audit_descriptor = {
  MYSQL_AUDIT_INTERFACE_VERSION,
  nullptr,  /* release_thd */
  AuditNotify,
  { [MYSQL_AUDIT_GENERAL_CLASS] = MYSQL_AUDIT_GENERAL_STATUS },
};

static int mysql_plugin_init(void *) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current_epoch = 0;
  g_initialized = false;
  return 0;
}

static int mysql_plugin_deinit(void *) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_toy_module.reset();
  g_backend.reset();
  g_coordinator.reset();
  g_state_store.reset();
  g_current_epoch = 0;
  g_initialized = false;
  return 0;
}

} // anonymous namespace

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

// ---- Txn UDF exports ----

__attribute__((visibility("default")))
bool mysql_accel_txn_init_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitTxnInit(initid, args, message);
}

__attribute__((visibility("default")))
long long mysql_accel_txn_init(UDF_INIT *initid, UDF_ARGS *args,
                                char *is_null, char *error) {
  return UdfTxnInit(initid, args, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_txn_capture_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitTxnCapture(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_txn_capture(UDF_INIT *initid, UDF_ARGS *args,
                               char *result, unsigned long *length,
                               unsigned char *is_null, unsigned char *error) {
  return UdfTxnCapture(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_txn_read_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitTxnRead(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_txn_read(UDF_INIT *initid, UDF_ARGS *args,
                            char *result, unsigned long *length,
                            unsigned char *is_null, unsigned char *error) {
  return UdfTxnRead(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_txn_epoch_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitTxnEpoch(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_txn_epoch(UDF_INIT *initid, UDF_ARGS *args,
                             char *result, unsigned long *length,
                             unsigned char *is_null, unsigned char *error) {
  return UdfTxnEpoch(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_txn_reset_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitTxnReset(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_txn_reset(UDF_INIT *initid, UDF_ARGS *args,
                             char *result, unsigned long *length,
                             unsigned char *is_null, unsigned char *error) {
  return UdfTxnReset(initid, args, result, length, is_null, error);
}

// ---- Mutations UDF exports ----

__attribute__((visibility("default")))
bool mysql_accel_mutations_capture_row_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsCaptureRow(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_capture_row(UDF_INIT *initid, UDF_ARGS *args,
                                         char *result, unsigned long *length,
                                         unsigned char *is_null, unsigned char *error) {
  return UdfMutationsCaptureRow(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_mutations_last_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsLast(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_last(UDF_INIT *initid, UDF_ARGS *args,
                                  char *result, unsigned long *length,
                                  unsigned char *is_null, unsigned char *error) {
  return UdfMutationsLast(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_mutations_drain_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsDrain(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_drain(UDF_INIT *initid, UDF_ARGS *args,
                                   char *result, unsigned long *length,
                                   unsigned char *is_null, unsigned char *error) {
  return UdfMutationsDrain(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_mutations_count_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitMutationsCount(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_mutations_count(UDF_INIT *initid, UDF_ARGS *args,
                                   char *result, unsigned long *length,
                                   unsigned char *is_null, unsigned char *error) {
  return UdfMutationsCount(initid, args, result, length, is_null, error);
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


// ---- Scan UDF exports ----

__attribute__((visibility("default")))
bool mysql_accel_scan_init_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitScanInit(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_scan_init(UDF_INIT *initid, UDF_ARGS *args,
                             char *result, unsigned long *length,
                             unsigned char *is_null, unsigned char *error) {
  return UdfScanInit(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_scan_exec_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitScanExec(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_scan_exec(UDF_INIT *initid, UDF_ARGS *args,
                             char *result, unsigned long *length,
                             unsigned char *is_null, unsigned char *error) {
  return UdfScanExec(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_scan_proj_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitScanProj(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_scan_proj(UDF_INIT *initid, UDF_ARGS *args,
                             char *result, unsigned long *length,
                             unsigned char *is_null, unsigned char *error) {
  return UdfScanProj(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_scan_filter_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitScanFilter(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_scan_filter(UDF_INIT *initid, UDF_ARGS *args,
                               char *result, unsigned long *length,
                               unsigned char *is_null, unsigned char *error) {
  return UdfScanFilter(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_scan_clear_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitScanClear(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_scan_clear(UDF_INIT *initid, UDF_ARGS *args,
                              char *result, unsigned long *length,
                              unsigned char *is_null, unsigned char *error) {
  return UdfScanClear(initid, args, result, length, is_null, error);
}

// ---- Runtime UDF exports ----

__attribute__((visibility("default")))
bool mysql_accel_runtime_init_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitRtInit(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_runtime_init(UDF_INIT *initid, UDF_ARGS *args,
                                char *result, unsigned long *length,
                                unsigned char *is_null, unsigned char *error) {
  return UdfRtInit(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_runtime_build_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitRtBuild(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_runtime_build(UDF_INIT *initid, UDF_ARGS *args,
                                 char *result, unsigned long *length,
                                 unsigned char *is_null, unsigned char *error) {
  return UdfRtBuild(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_runtime_execute_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitRtExecute(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_runtime_execute(UDF_INIT *initid, UDF_ARGS *args,
                                   char *result, unsigned long *length,
                                   unsigned char *is_null, unsigned char *error) {
  return UdfRtExecute(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_runtime_read_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitRtRead(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_runtime_read(UDF_INIT *initid, UDF_ARGS *args,
                                char *result, unsigned long *length,
                                unsigned char *is_null, unsigned char *error) {
  return UdfRtRead(initid, args, result, length, is_null, error);
}

__attribute__((visibility("default")))
bool mysql_accel_runtime_reset_init(UDF_INIT *initid, UDF_ARGS *args, char *message) {
  return UdfInitRtReset(initid, args, message);
}

__attribute__((visibility("default")))
char *mysql_accel_runtime_reset(UDF_INIT *initid, UDF_ARGS *args,
                                 char *result, unsigned long *length,
                                 unsigned char *is_null, unsigned char *error) {
  return UdfRtReset(initid, args, result, length, is_null, error);
}


} // extern "C"
