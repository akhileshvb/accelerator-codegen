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
  // One char per argument: 'i' int, 'f' double, 'd' date, anything else string.
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

std::string ResolveCostModelPath(const char *env_name, const char *default_relative_path);
int32_t ParseUnixDays(const std::string &text);
std::string MysqlCliPrefix();
std::string ShellEscape(const std::string &s);
std::vector<std::string> SplitTsv(const std::string &line);
// Runs `sql` through the mysql CLI and calls `callback` with each row's columns.
void ScanTypedTuples(const std::string &sql,
                     const std::function<void(const std::vector<std::string> &)> &callback);
const std::string &Col(const std::vector<std::string> &cols, int attribute);
int64_t TypedInt(const std::vector<std::string> &cols, int attribute);
int32_t TypedDate(const std::vector<std::string> &cols, int attribute);
std::string TypedText(const std::vector<std::string> &cols, int attribute);
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
    if (decimal.scale > 0) {
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
    return "";
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
    coordinator.Register(std::move(module_view));
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
    while (coordinator.HasPendingWork() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  // Build the state from the tables when there is none or it is invalid.
  // One Runtime serves every session, and MySQL commits the aux state after
  // the write is visible (audit event), so a build may race a write: it is
  // installed only if no write was in flight during it and nothing was
  // published meanwhile (else it would lose that write, or count it twice).
  // A declined build leaves the state as it was: the query reads it if it is
  // valid, else answers from SQL (ExecuteFallback), and a later query
  // rebuilds.
  // Returns whether this call built the state. A write in flight is waited
  // out and a build that raced a write is retried; only when the state still
  // cannot be installed after kMaxBuildAttempts does the query answer from
  // SQL (the last resort).
  bool RebuildIfNeeded() {
    if (built.load() && !store.NeedsRebuild()) {
      return false;
    }
    std::lock_guard<std::mutex> lock(rebuild_mutex);
    for (int attempt = 0; attempt < kMaxBuildAttempts; ++attempt) {
      if (built.load() && !store.NeedsRebuild()) {
        return false; // another session rebuilt it
      }
      while (coordinator.HasPendingWork()) {
        // A write is between capture and its commit: wait for it.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      const auto before = store.ChangeCount();
      Builder builder;
      module.Build(backend, builder);
      if (!builder.finished) {
        throw std::runtime_error("auxiliary build did not finish");
      }
      if (coordinator.HasPendingWork() ||
          !store.InstallIfUnchanged(std::move(builder.images), accelerator, before)) {
        continue; // a write landed during the build: build again
      }
      store.ClearTransactionDirty();
      built = true;
      accelerator_codegen::aux_state::engine::mysql::
          RegisterMysqlBackendForMutations();
      return true;
    }
    return false;
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
    module.Execute(backend, arguments, sink);
    core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                           : rebuilt         ? core::AuxExecutePath::RebuiltOnRead
                                             : core::AuxExecutePath::Hit);
  }

private:
  MysqlStateStore store;
  core::AuxTransactionCoordinator coordinator;
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
