// MySQL engine backend: the public interface that per-query code plugs into.
//
// The backend (this header, runtime.cpp, mysql_plugin.cpp) names NO query.
// Everything query-specific lives under src/generated/<bench>/q<N>/mysql/ and
// reaches the backend through registries with static registrars:
//
//   * UDF registry (this header): src/generated/<bench>/q<N>/mysql/plugin/runtime.cpp
//     defines the query's scans, Runtime<> instance and entry point and registers
//     it with a file-scope MysqlUdfRegistrar.  The same dir's udf.cpp exports the
//     UDF symbols the MySQL server resolves by name (ACCEL_MYSQL_QUERY_UDF in
//     udf_shim.hpp), and functions.sql holds its CREATE FUNCTION statement
//     (scripts/mysql_register_functions.sh assembles the fragments).
//   * Query catalog (catalog/mysql/query_catalog.hpp): q<N>/mysql/aux/catalog.cpp
//     registers the query's catalog entry (MysqlQueryCatalogRegistrar).
//
// Registries are plain functions with function-local static storage in
// runtime.cpp / query_catalog.cpp; there are no header-only singletons.
//
// Engine-level helpers (ScanTypedTuples, Typed*, ByteExactLiteral,
// KeyListPredicate, ColumnsCompareBytewise, Runtime<>) stay in the backend and
// are exported in namespace detail below for per-query code.
#pragma once

#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_calibration.hpp"
#include "accelerator_codegen/stateful/core/aux_execute_stats.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_aux_backend.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_state_store.hpp"
#include "accelerator_codegen/stateful/mysql/mysql_xact_callbacks.hpp"

extern "C" int mysql_accel_get_observer_mutation_count();

namespace accelerator_codegen::aux_state::engine::mysql {

// ---- UDF registry ---------------------------------------------------------
struct MysqlUdfEntry {
  // SQL function name; the UDF symbols are <name>, <name>_init, <name>_deinit.
  std::string name;
  // One char per argument: 'i' int (int64), 'f' double (nearest double of the
  // argument's decimal text), 'n' exact decimal (ScaledDecimal: 0.065 is
  // {65, 3}, never a double), 'd' date, anything else string.  A NULL
  // argument arrives as an AuxValue with is_null set (of the declared type);
  // a malformed int/decimal/double/date argument is a UDF error (SQL NULL
  // result), never a default value.
  std::string arg_types;
  // Runs the query; returns '|'-separated rows, one per line.
  std::string (*execute)(const core::QueryArguments &) = nullptr;
  // A NULL first argument yields an empty result instead of running the query.
  bool null_first_arg_yields_empty = false;
};

// Registers (replacing any entry of the same name).  Entries are held stably.
void RegisterMysqlUdf(MysqlUdfEntry entry);
// nullptr if no query registered under `name`.
const MysqlUdfEntry *FindMysqlUdf(const std::string &name);
// Registered UDF names, sorted.
std::vector<std::string> RegisteredMysqlUdfNames();
// Runs the registered UDF `name` (throws std::runtime_error if unknown).
std::string ExecuteMysqlUdf(const std::string &name, const core::QueryArguments &arguments);

struct MysqlUdfRegistrar {
  explicit MysqlUdfRegistrar(MysqlUdfEntry entry) { RegisterMysqlUdf(std::move(entry)); }
};

namespace detail {

namespace core = accelerator_codegen::aux_state::core;

// Table scans this thread ran (a query that scans answered from SQL).
extern thread_local uint64_t t_table_scans;

// Process-wide build counters (mysql_accel_get_build_stats).
struct BuildStats {
  std::atomic<uint64_t> snapshot_builds{0};  // installed from a consistent snapshot
  std::atomic<uint64_t> restart_builds{0};   // installed by the restart-on-write scheme
  std::atomic<uint64_t> replayed_batches{0}; // writes recorded during snapshot builds and replayed
  std::atomic<uint64_t> build_retries{0};    // builds discarded and redone
};
BuildStats &GetBuildStats();

std::string ResolveCostModelPath(const char *env_name, const char *default_relative_path);
// The database the scan connections use: ACCEL_MYSQL_DB / MYSQL_DATABASE from
// the server's environment, or this override (mysql_accel_set_scan_database).
// Aux states already built are not affected: invalidate them to rebuild.
void SetScanDatabase(const std::string &db);
// "cli": scan through the mysql CLI (no snapshot builds); "client": in-process
// C API when available; anything else: decide from ACCEL_MYSQL_SCAN / availability.
void SetScanMode(const std::string &mode);
// Strict 'YYYY-MM-DD' -> Unix-epoch days; throws std::runtime_error if malformed.
int32_t ParseUnixDays(const std::string &text);
// `mysql` CLI command line (connection options only) for the server's scan
// connection settings; append further options and the statement.
std::string MysqlCliBase();
std::string MysqlCliPrefix();
std::string ShellEscape(const std::string &s);
std::vector<std::string> SplitTsv(const std::string &line);

// Streams the result of `sql` and calls `callback` once per row with the row's
// columns as text.  In-process over the MySQL C API (mysql_use_result, rows
// are not buffered) when the plugin is built with the client library
// (ACCEL_MYSQL_HAVE_CLIENT), else through `mysql --quick`.  The vector passed
// to the callback is reused for the next row (copy what you keep).  Inside a
// SnapshotScanSession on this thread every scan runs on the session's
// connection (one consistent read view); otherwise each call uses its own
// connection (autocommit, sees committed data at the time of the call).
void ScanTypedTuples(const std::string &sql,
                     const std::function<void(const std::vector<std::string> &)> &callback);

// A consistent read view for a set of scans: opens a connection with
// START TRANSACTION WITH CONSISTENT SNAPSHOT (REPEATABLE READ, read only) and
// routes this thread's ScanTypedTuples through it until destroyed (the
// connection is closed, which ends the view; no COMMIT/ROLLBACK statement is
// issued because the audit plugin reads those as transaction events).
// Throws std::runtime_error if the connection or snapshot fails and
// std::logic_error if !SnapshotScansSupported() or one is already open.
class SnapshotScanSession {
public:
  SnapshotScanSession();
  ~SnapshotScanSession();
  SnapshotScanSession(const SnapshotScanSession &) = delete;
  SnapshotScanSession &operator=(const SnapshotScanSession &) = delete;
  void Close();
};
// False when built without the client library (CLI scans cannot hold a view).
bool SnapshotScansSupported();

const std::string &Col(const std::vector<std::string> &cols, int attribute);
// Whether the column of the row being delivered by ScanTypedTuples is SQL NULL.
bool TypedIsNull(const std::vector<std::string> &cols, int attribute);
// The typed accessors parse the text exactly and throw std::runtime_error on
// text they cannot represent (malformed, out of range, needs rounding); a NULL
// cell reads as 0 / "" (see TypedIsNull).
int64_t TypedInt(const std::vector<std::string> &cols, int attribute);
int32_t TypedDate(const std::vector<std::string> &cols, int attribute);
std::string TypedText(const std::vector<std::string> &cols, int attribute);
// The decimal exactly as stored, at its own scale ("0.065" -> {65, 3}).
core::ScaledDecimal TypedScaledDecimal(const std::vector<std::string> &cols, int attribute);
// The decimal as an integer at `scale` (value * 10^scale); throws unless that
// is exact (no digit is ever rounded away).
int64_t TypedDecimal(const std::vector<std::string> &cols, int attribute, int scale);
// TypedDecimal(cols, attribute, 2): the raw value at scale 2 (the contract
// existing callers rely on).
int64_t TypedDecimalRaw(const std::vector<std::string> &cols, int attribute);
char TypedChar(const std::vector<std::string> &cols, int attribute);
// A text value as an SQL literal that reaches the server byte for byte.
std::string ByteExactLiteral(const std::string &text);
// `column IN (k1, k2, ...)`, or FALSE for no keys.
std::string KeyListPredicate(const char *column, const std::vector<int64_t> &keys);
// Whether every (table, column) compares text byte by byte (utf8mb4_0900_bin).
bool ColumnsCompareBytewise(const std::vector<std::pair<std::string, std::string>> &columns);

class Builder final : public core::StateBuilder {
public:
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

class TsvSink final : public core::AuxRowSink {
public:
  explicit TsvSink(std::string &out_p) : out(out_p) {}

  void Begin(const core::OutputSchema &) override {}

  void WriteRow(const core::AuxRow &row) override {
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (column > 0) {
        out.push_back('|');
      }
      // NULL as the mysql client prints it (an all-NULL row stays a row).
      out += row.values[column].is_null ? std::string("NULL") : CellText(row.values[column]);
    }
    out.push_back('\n');
  }

  void End() override {}

private:
  static std::string DecimalText(const core::ScaledDecimal &decimal) {
    const bool negative = decimal.value < 0;
    uint64_t magnitude = negative
                            ? static_cast<uint64_t>(-(decimal.value + 1)) + 1
                            : static_cast<uint64_t>(decimal.value);
    std::string digits = std::to_string(magnitude);
    if (decimal.scale < 0) {
      // value * 10^-scale, exactly (a negative scale is trailing zeros).
      if (magnitude != 0) digits.append(static_cast<std::size_t>(-decimal.scale), '0');
    } else if (decimal.scale > 0) {
      if (digits.size() <= static_cast<std::size_t>(decimal.scale)) {
        digits.insert(0, static_cast<std::size_t>(decimal.scale) + 1 -
                               digits.size(),
                     '0');
      }
      digits.insert(digits.size() - decimal.scale, 1, '.');
    }
    if (negative) {
      digits.insert(digits.begin(), '-');
    }
    return digits;
  }

  static std::string FormatUnixDate(int32_t days) {
    int z = days + 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    return buf;
  }

  static std::string CellText(const core::AuxValue &value) {
    if (std::holds_alternative<int64_t>(value.payload)) {
      return std::to_string(std::get<int64_t>(value.payload));
    }
    if (std::holds_alternative<uint64_t>(value.payload)) {
      return std::to_string(std::get<uint64_t>(value.payload));
    }
    if (std::holds_alternative<double>(value.payload)) {
      std::ostringstream ss;
      ss.setf(std::ios::fixed);
      ss.precision(6);
      ss << std::get<double>(value.payload);
      return ss.str();
    }
    if (std::holds_alternative<core::ScaledDecimal>(value.payload)) {
      return DecimalText(std::get<core::ScaledDecimal>(value.payload));
    }
    if (std::holds_alternative<core::AuxDate>(value.payload)) {
      return FormatUnixDate(std::get<core::AuxDate>(value.payload).days_since_epoch);
    }
    if (std::holds_alternative<std::string>(value.payload)) {
      return std::get<std::string>(value.payload);
    }
    if (std::holds_alternative<bool>(value.payload)) {
      return std::get<bool>(value.payload) ? "t" : "f";
    }
    // Every payload alternative is handled above; fail loudly if one is added.
    throw std::logic_error("unprintable AuxValue payload");
  }

  std::string &out;
};

template <typename Module, typename Config, typename ScanT>
class Runtime final {
public:
  Runtime(const char *cost_model_env, const char *default_config_path,
          core::AcceleratorId accelerator, Config config)
      : store(), coordinator(),
        backend(
            coordinator, store,
            [] { return core::VisibilityToken{}; },
            [cost_model_env, default_config_path] {
              return core::LoadAuxCostModelJson(
                  ResolveCostModelPath(cost_model_env, default_config_path));
            }),
        scan(), module(config, &scan), accelerator(std::move(accelerator)) {
    auto module_view = std::shared_ptr<Module>(&module, [](Module *) {});
    replay_coordinator.Register(module_view);
    coordinator.Register(module_view);
    replay_coordinator.AttachServices(backend);
    // Every session (connection) stages its writes in a coordinator of its own
    // over this same module: one session's ROLLBACK or COMMIT never touches
    // another's staged writes (see MysqlAuxBackend, "Per-session transactions").
    backend.SetSessionCoordinatorFactory([module_view] {
      auto session_coordinator = std::make_unique<core::AuxTransactionCoordinator>();
      session_coordinator->Register(module_view);
      return session_coordinator;
    });
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlMutationBackend(&backend);
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlXactCallbacks();
  }

  void WaitForCapturedWrites() {
    static const auto limit = std::chrono::microseconds([] {
      const char *value = std::getenv("ACCEL_AUX_PENDING_WAIT_US");
      return value && *value ? std::strtoll(value, nullptr, 10) : 500000LL;
    }());
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (backend.HasPendingWork() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  // Build the state from the tables when there is none or it is invalid.
  // One Runtime serves every session, and MySQL commits the aux state after
  // the write is visible (audit event), so a build races writes.
  //
  // Snapshot build (the plugin is built with the client library): the scans
  // run in one consistent read view (SnapshotScanSession) taken while no
  // captured write is pending; writes that commit meanwhile are recorded
  // (MysqlAuxBackend::BeginBuildLog), and at install, under the commit mutex
  // and the readers' gate, the build is installed and the recorded writes are
  // replayed on it through the normal maintenance path (a private coordinator
  // over the same module, so other sessions' open transactions are not
  // touched). A write is therefore never lost or counted twice (it is either
  // in the view or in the log: see BeginBuildLog), and a build is never
  // restarted because of concurrent writes. It is retried (bounded) only if a
  // write was captured while the view was being opened, a captured write
  // failed to decode, or the log outgrew its limit.
  //
  // Without the client library the scans cannot share a view: the build is
  // discarded and restarted whenever a write lands during it (bounded by
  // kMaxBuildAttempts).
  //
  // In both cases, when the state cannot be installed (a long-open writer
  // transaction, retries exhausted) the call returns false and the query
  // answers from SQL (the module's fallback); a later query rebuilds.
  // Returns whether this call built the state.
  bool RebuildIfNeeded() {
    if (built.load() && !store.NeedsRebuild()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(rebuild_mutex);
    return SnapshotScansSupported() ? RebuildFromSnapshotLocked() : RebuildByRestartLocked();
  }

  void Execute(const core::QueryArguments &arguments, std::string &out) {
    // Every read in this statement: one aux state version.
    accelerator_codegen::aux_state::AuxReadScope one_version;
    // Prefer this Runtime for subsequent trigger captures in this session.
    accelerator_codegen::aux_state::engine::mysql::
        RegisterMysqlMutationBackend(&backend);

    // Captured writes are committed to the aux state by the writing
    // session's audit event, after the write is already visible (never
    // here: this may be another session, and the write may not be committed
    // yet). Until then this accelerator's commit is pending: wait for it, as
    // a reader waits for a pending commit (bounded like the store's wait).
    WaitForCapturedWrites();
    const bool rebuilt = RebuildIfNeeded();
    observed_mutations = mysql_accel_get_observer_mutation_count();
    TsvSink sink(out);
    t_table_scans = 0;
    // An install (state + replayed writes) is atomic for readers.
    std::shared_lock<accelerator_codegen::aux_state::WriterPreferringSharedMutex> reading(read_gate);
    module.Execute(backend, arguments, sink);
    core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                           : rebuilt         ? core::AuxExecutePath::RebuiltOnRead
                                             : core::AuxExecutePath::Hit);
  }

private:
  // Bounded wait for captured writes to commit (an open writer transaction
  // may hold them pending for long): false on timeout.
  bool WaitForNoPendingWork() {
    static const auto limit = std::chrono::microseconds([] {
      const char *value = std::getenv("ACCEL_AUX_BUILD_WAIT_US");
      return value && *value ? std::strtoll(value, nullptr, 10) : 10000000LL;
    }());
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (backend.HasPendingWork()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  }

  void FinishInstall() {
    store.ClearTransactionDirty();
    built = true;
    accelerator_codegen::aux_state::engine::mysql::RegisterMysqlBackendForMutations();
  }

  bool RebuildFromSnapshotLocked() {
    namespace mysql_ns = accelerator_codegen::aux_state::engine::mysql;
    for (int attempt = 0; attempt < kMaxBuildAttempts; ++attempt) {
      if (built.load() && !store.NeedsRebuild()) {
        return false; // another session rebuilt it
      }
      if (!WaitForNoPendingWork()) {
        return false;
      }
      const uint64_t staged = backend.StagedMutationCount();
      Builder builder;
      {
        SnapshotScanSession view; // the read view; closed when this block ends
        if (!backend.BeginBuildLog(staged)) {
          ++GetBuildStats().build_retries;
          continue; // a write was captured around the view's start: take another
        }
        try {
          module.Build(backend, builder);
        } catch (...) {
          backend.EndBuildLog();
          throw;
        }
        if (!builder.finished) {
          backend.EndBuildLog();
          throw std::runtime_error("auxiliary build did not finish");
        }
      }
      // Install atomically wrt commits of captured writes and readers.
      std::lock_guard<std::mutex> commits(backend.CommitMutex());
      std::lock_guard<std::mutex> module_calls(backend.ModuleMutex()); // replay uses the module
      std::unique_lock<accelerator_codegen::aux_state::WriterPreferringSharedMutex> readers(read_gate);
      auto log = backend.EndBuildLog();
      if (!log.complete) {
        ++GetBuildStats().build_retries;
        continue; // lost or too many writes during the build: build again
      }
      ++GetBuildStats().snapshot_builds;
      GetBuildStats().replayed_batches += log.batches.size();
      store.Install(std::move(builder.images), accelerator);
      if (!log.batches.empty()) {
        try {
          for (const auto &batch : log.batches) {
            replay_coordinator.OnMutation(batch);
          }
          replay_coordinator.PreCommit();
          replay_coordinator.Commit();
        } catch (...) {
          // Not replayed: the state is not trustworthy.
          try { replay_coordinator.Abort(); } catch (...) {}
          store.Invalidate(core::AllPartitionsPlan());
          ++GetBuildStats().build_retries;
          continue;
        }
      }
      FinishInstall();
      return true;
    }
    return false;
  }

  bool RebuildByRestartLocked() {
    for (int attempt = 0; attempt < kMaxBuildAttempts; ++attempt) {
      if (built.load() && !store.NeedsRebuild()) {
        return false; // another session rebuilt it
      }
      if (!WaitForNoPendingWork()) {
        return false;
      }
      const auto before = store.ChangeCount();
      Builder builder;
      module.Build(backend, builder);
      if (!builder.finished) {
        throw std::runtime_error("auxiliary build did not finish");
      }
      if (backend.HasPendingWork() ||
          !store.InstallIfUnchanged(std::move(builder.images), accelerator, before)) {
        ++GetBuildStats().build_retries;
        continue; // a write landed during the build: build again
      }
      ++GetBuildStats().restart_builds;
      FinishInstall();
      return true;
    }
    return false;
  }

  MysqlStateStore store;
  core::AuxTransactionCoordinator coordinator;
  // Replays the writes a snapshot build recorded; same module, own
  // transaction buffers.
  core::AuxTransactionCoordinator replay_coordinator;
  // Readers share it; the install takes it exclusively (writer-preferring, so
  // a stream of reads cannot starve an install).
  accelerator_codegen::aux_state::WriterPreferringSharedMutex read_gate;
  accelerator_codegen::aux_state::engine::mysql::MysqlAuxBackend backend;
  ScanT scan;
  Module module;
  std::string accelerator;
  static constexpr int kMaxBuildAttempts = 64;
  std::mutex rebuild_mutex;
  std::atomic<bool> built{false};
  std::atomic<int> observed_mutations{0};
};


} // namespace detail
} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
