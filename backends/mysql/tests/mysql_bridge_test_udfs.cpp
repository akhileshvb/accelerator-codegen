// Gate-test UDFs of the MySQL plugin. Built only into the test plugin variant
// (cmake -DACCEL_MYSQL_TEST_UDFS=ON; scripts/build_accel_mysql_plugin.sh with
// ACCEL_MYSQL_PLUGIN_VARIANT=test), never into the plugin experiments load.
//
// The layer harnesses in backends/mysql/tests/ call these through SQL:
//   txn:       mysql_accel_txn_{init,capture,read,epoch,reset} — a toy accelerator
//              registered in MysqlHost; its captures reach the host like a trigger's
//              and are committed / aborted by the audit hook.
//   mutations: mysql_accel_mutations_{last,drain,count} — inspect the rows
//              the trigger capture UDF decoded (through the bridge observer,
//              backends/mysql/engine/mysql_bridge.hpp).
//   scan:      mysql_accel_scan_{init,exec,proj,filter,clear} — create and seed
//              the table toy_scan_table and scan it through ScanMysqlTableForRebuild.
//   runtime:   mysql_accel_runtime_{init,build,execute,read,reset} — a toy
//              accelerator over a real table (host + scan helper) through SQL.

#include "backends/mysql/engine/plugin_minimal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "backends/mysql/api/mysql_host.hpp"
#include "backends/mysql/api/runtime.hpp"
#include "backends/mysql/engine/mysql_bridge.hpp"
#include "backends/mysql/engine/mysql_session.hpp"
#include "backends/mysql/engine/scan_helper.hpp"

namespace accel = accelerator_codegen::aux_state;
namespace core = accel::core;
namespace engine = accel::engine::mysql;
namespace runtime = accel::runtime;
namespace maintenance = accel::maintenance;

namespace {

struct ToyPayload {
  int count;
  std::string label;
};

// Depends on one table, under the module's own number for it.
class TxnToyModule final : public core::AuxQueryModule {
public:
  TxnToyModule(core::AcceleratorId id, core::TableId table) : id(std::move(id)), table(table) {}

  core::AcceleratorId Id() const override { return id; }

  std::vector<core::TableDependency> Dependencies() const override {
    core::TableDependency dep;
    dep.table = table;
    return {dep};
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<core::BasicAuxTxnBuffer>();
  }

  // Starts with no image: the first commit's maintenance publishes it.
  void Build(core::AuxEngineServices &, core::StateBuilder &builder) override {
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

private:
  core::AcceleratorId id;
  core::TableId table;
};

// ---- Captured batches (mutations inspection UDFs) ----

std::vector<core::MutationBatch> g_captured_mutation_batches;
std::mutex g_mutations_mutex;

// ---- Toy accelerators, registered in MysqlHost on first init ----

// The accelerator of `module`, registered once (the host cannot unregister).
runtime::AcceleratorRuntime &Registered(const std::shared_ptr<core::AuxQueryModule> &module,
                                        const runtime::TableMap &tables = {}) {
  auto &host = engine::MysqlHost::Get();
  if (auto *existing = host.Runtime().Find(module->Id())) {
    return *existing;
  }
  return host.Register(module, maintenance::CostModelParams{}, tables);
}

// Rebuilds the accelerator's state from its module.
void Rebuild(runtime::AcceleratorRuntime &accelerator) {
  accelerator.Invalidate();
  if (!accelerator.BuildNow()) {
    throw std::runtime_error("toy build did not sync");
  }
}

std::shared_ptr<TxnToyModule> g_toy_module = std::make_shared<TxnToyModule>("txn_toy", 1);
// The same table under another module number (9): TableMap maps it to the host's 1.
constexpr core::TableId kAliasModuleTable = 9;
std::shared_ptr<TxnToyModule> g_alias_module =
    std::make_shared<TxnToyModule>("txn_toy_alias", kAliasModuleTable);
runtime::AcceleratorRuntime *g_txn_accelerator = nullptr;
runtime::AcceleratorRuntime *g_alias_accelerator = nullptr;
uint64_t g_txn_baseline = 0; // commits applied before the last init
uint64_t g_alias_baseline = 0;
bool g_initialized = false;
std::mutex g_mutex;

runtime::ReadOptions WaitingRead() {
  runtime::ReadOptions options;
  options.max_wait_us = 10'000'000;
  return options;
}

uint64_t TxnEpoch() { return g_txn_accelerator->Stats().commits_applied - g_txn_baseline; }

// ---- Bridge observer: the capture UDF reports the rows it decoded here ----

class TestBridgeObserver final : public engine::MysqlBridgeObserver {
public:
  void OnCapturedRow(const engine::CapturedRow &row) override {
    engine::StatementCapture capture;
    capture.Add(row, {});
    std::lock_guard<std::mutex> lock(g_mutations_mutex);
    for (const auto &batch : capture.Take()) {
      g_captured_mutation_batches.push_back(runtime::ToMutationBatch(*batch, {}));
    }
  }

  void OnCommit() override {}
  void OnRollback() override {}
  void OnPluginDeinit() override {}
};

TestBridgeObserver g_test_observer;

// Installed when the plugin .so is loaded (static initialization).
struct InstallTestBridgeObserver {
  InstallTestBridgeObserver() { engine::SetMysqlBridgeObserver(&g_test_observer); }
  ~InstallTestBridgeObserver() { engine::SetMysqlBridgeObserver(nullptr); }
} g_install_test_bridge_observer;

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
                            char *, char *error) {
  try {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_txn_accelerator = &Registered(g_toy_module);
    g_alias_accelerator = &Registered(g_alias_module, {{kAliasModuleTable, 1}});
    Rebuild(*g_txn_accelerator);
    Rebuild(*g_alias_accelerator);
    g_txn_baseline = g_txn_accelerator->Stats().commits_applied;
    g_alias_baseline = g_alias_accelerator->Stats().commits_applied;
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
    if (!g_initialized) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }

    // One row of (int64 value, string label); NULL arguments are NULL cells.
    const std::string value = ReadUdfStringArg(args, 2);
    const std::string label = ReadUdfStringArg(args, 3);
    engine::CapturedRow row;
    row.table = static_cast<core::TableId>(ReadUdfIntArg(args, 0, 0));
    row.kind = static_cast<core::MutationKind>(ReadUdfIntArg(args, 1, 0));
    row.types = "is";
    row.cells = {{args->args[2] ? value.data() : nullptr, value.size()},
                 {args->args[3] && label != "NULL" ? label.data() : nullptr, label.size()}};
    engine::MysqlHost::Get().OnCapturedRow(engine::CurrentMysqlSession(), row);

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
    if (!g_initialized) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }

    auto read = g_txn_accelerator->BeginRead(WaitingRead());
    if (!read.Servable()) {
      throw std::runtime_error("toy state not servable");
    }
    auto snapshot = g_txn_accelerator->StateStore().Read(g_txn_accelerator->CurrentVisibility());
    std::ostringstream oss;
    oss << "epoch=" << TxnEpoch()
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
  const uint64_t alias = g_initialized ? g_alias_accelerator->Stats().commits_applied - g_alias_baseline : 0;
  const std::string text = "epoch=" + std::to_string(g_initialized ? TxnEpoch() : 0) +
                           " alias_epoch=" + std::to_string(alias);
  std::memcpy(result, text.c_str(), text.size() + 1);
  *length = text.size();
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
  g_initialized = false;
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

// ---- Mutations inspection UDFs ----

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
    std::string cmd = engine::detail::MysqlCliBase() +
                      " --batch --raw -e \"" + sql + "\" 2>/dev/null";

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
    std::string drop_cmd = engine::detail::MysqlCliBase() +
                           " -e \"" + drop + "\" 2>/dev/null";
    system(drop_cmd.c_str());

    std::string create = "CREATE TABLE toy_scan_table ("
                         "id INT, name VARCHAR(100), score INT)";
    std::string create_cmd = engine::detail::MysqlCliBase() +
                             " -e \"" + create + "\" 2>/dev/null";
    system(create_cmd.c_str());

    std::string insert = "INSERT INTO toy_scan_table VALUES "
                         "(1, \"Alice\", 100), "
                         "(2, \"Bob\", NULL), "
                         "(3, \"Charlie\", 300)";
    std::string insert_cmd = engine::detail::MysqlCliBase() +
                             " -e \"" + insert + "\" 2>/dev/null";
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
    std::string cmd = engine::detail::MysqlCliBase() +
                      " -e \"" + drop + "\" 2>/dev/null";
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
    dep.table = 2; // not the txn toy's table 1: captures reach one toy only
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

std::shared_ptr<RuntimeToyModule> g_rt_module = std::make_shared<RuntimeToyModule>();
runtime::AcceleratorRuntime *g_rt_accelerator = nullptr;
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

    // Register the real MySQL CLI scanner for host-layer scans.
    engine::SetMysqlTableScanner(std::make_shared<RealMysqlCliScanner>());

    g_rt_module->table_name = ReadUdfStringArg(args, 0);
    g_rt_module->columns = {"id", "name", "val"};
    g_rt_accelerator = &Registered(g_rt_module);
    g_rt_accelerator->Invalidate(); // empty until built
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
    if (!g_rt_initialized) {
      std::strcpy(result, "not_initialized");
      *length = 16;
      return result;
    }
    Rebuild(*g_rt_accelerator);
    std::ostringstream oss;
    oss << "build=ok epoch=" << g_rt_accelerator->Stats().builds;
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
    if (!g_rt_initialized) {
      *is_null = 1;
      return result;
    }

    StringCollectorSink sink;
    core::QueryArguments args;
    auto read = g_rt_accelerator->BeginRead(WaitingRead());
    if (!read.Servable()) {
      throw std::runtime_error("toy state not servable");
    }
    g_rt_module->Execute(*g_rt_accelerator, args, sink);

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
    if (!g_rt_initialized) {
      *is_null = 1;
      return result;
    }

    // No read gate: before the first build the state is empty, not awaited.
    auto snapshot = g_rt_accelerator->IsSynced()
                        ? g_rt_accelerator->StateStore().Read(g_rt_accelerator->CurrentVisibility())
                        : core::StateSnapshot{};
    std::ostringstream oss;
    oss << "epoch=" << g_rt_accelerator->Stats().builds
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
  g_rt_initialized = false;
  std::strcpy(result, "ok");
  *length = 2;
  return result;
}

} // anonymous namespace

extern "C" {

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

// ---- Mutations inspection UDF exports ----

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
