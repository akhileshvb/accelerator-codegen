// GENERATED BACKEND
#if defined(ACCELERATOR_CODEGEN_MYSQL_PLUGIN)

#include "backends/mysql/api/mysql_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "backends/mysql/api/runtime.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/maintenance/maintenance_action.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {
namespace {

using runtime::Epoch;

// How long a build waits for the writers in flight to finish before it
// gives up (the readers then answer from SQL and a later read tries again).
std::chrono::microseconds BuildWait() {
  static const auto wait = std::chrono::microseconds([] {
    const char *value = std::getenv("ACCEL_AUX_BUILD_WAIT_US");
    return value && *value ? std::strtoll(value, nullptr, 10) : 10'000'000LL;
  }());
  return wait;
}

// Connections one build may hold (a partitioned scan runs one partition at a
// time per connection); ACCEL_MYSQL_SCAN_LANES.
std::size_t MaxScanLanes() {
  static const std::size_t lanes = [] {
    const char *value = std::getenv("ACCEL_MYSQL_SCAN_LANES");
    const long long parsed = value && *value ? std::strtoll(value, nullptr, 10) : 8;
    return static_cast<std::size_t>(std::max(1LL, parsed));
  }();
  return lanes;
}

// Rows per captured table, from information_schema (InnoDB's estimate; the
// statistics cache is bypassed). A table is found by its capture triggers,
// which carry the host's id for it. Reloaded at most every kRefresh; a failed
// load keeps the last counts.
class TableRowCounts {
public:
  uint64_t Rows(core::TableId table) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!loaded || now - loaded_at >= kRefresh) {
      loaded = true;
      loaded_at = now;
      Load();
    }
    const auto it = rows.find(table);
    return it == rows.end() ? 0 : it->second;
  }

private:
  static constexpr std::chrono::seconds kRefresh{10};

  void Load() {
    static const std::string kCaptureCall = "mysql_accel_capture_row('";
    std::unordered_map<core::TableId, uint64_t> fresh;
    try {
      detail::ScanTypedTuples(
          "SELECT /*+ SET_VAR(information_schema_stats_expiry=0) */ t.table_rows, g.action_statement "
          "FROM information_schema.triggers g JOIN information_schema.tables t "
          "ON t.table_schema = g.event_object_schema AND t.table_name = g.event_object_table "
          "WHERE g.trigger_schema = DATABASE() AND g.action_statement LIKE '%mysql_accel_capture_row%'",
          [&](const std::vector<std::string> &cols) {
            const auto call = cols[1].find(kCaptureCall);
            if (call != std::string::npos) {
              const auto id = std::strtoull(cols[1].c_str() + call + kCaptureCall.size(), nullptr, 10);
              fresh[static_cast<core::TableId>(id)] = std::strtoull(cols[0].c_str(), nullptr, 10);
            }
          });
    } catch (const std::exception &) {
      return;
    }
    rows = std::move(fresh);
  }

  std::mutex mutex;
  bool loaded = false;
  std::chrono::steady_clock::time_point loaded_at;
  std::unordered_map<core::TableId, uint64_t> rows;
};

// The build's read: one consistent view of the database, routing this
// thread's scans through it until destroyed. Partitioned scans get a
// connection per build thread, all in the same view.
struct MysqlBuildSnapshot final : runtime::BuildSnapshot {
  MysqlBuildSnapshot(Epoch epoch_p, std::size_t lanes) : view(lanes), epoch(epoch_p) {}
  Epoch SnapshotEpoch() const override { return epoch; }
  detail::SnapshotScanSession view;
  Epoch epoch;
};

} // namespace

struct MysqlHost::Impl final : runtime::EngineSlots {
  // A session with captured work: its open statement's rows, and whether its
  // transaction can no longer be followed write by write.
  struct Writer {
    StatementCapture statement;
    bool poisoned = false; // ROLLBACK TO SAVEPOINT: undone writes are not tracked
    std::unordered_map<core::TableId, std::optional<std::vector<int32_t>>> watched;
    uint64_t watched_version = 0;
  };

  Impl() : runtime(*this, runtime::ResolveThreadBudget(0)) { SetThreads({}); }

  void SetThreads(const ThreadSettings &settings) {
    auto &threads = runtime.Threads();
    threads.SetBudget(runtime::ResolveThreadBudget(settings.budget));
    threads.SetPhaseLimits(runtime::ResolveReadThreads(settings.read, threads.Budget()),
                           runtime::ResolveBuildThreads(settings.build, threads.Budget()),
                           runtime::ResolveMaintenanceThreads(settings.maintenance, threads.Budget()));
  }

  mutable std::mutex mutex; // writers, opening, cost_models
  std::condition_variable changed;
  // Every session between its first captured row (of any table) and the end
  // of its transaction. A build opens its view only when there is none, so
  // every commit is either in the view or applied after it.
  std::unordered_map<Session, std::unique_ptr<Writer>> writers;
  int opening = 0; // views being opened: no session becomes a writer meanwhile
  std::map<core::AcceleratorId, maintenance::CostModelParams> cost_models;
  mutable TableRowCounts table_rows;
  std::atomic<Epoch> epoch{0}; // the newest commit's
  std::atomic<uint64_t> captured_rows{0};
  std::atomic<const runtime::AcceleratorRuntime *> last_read{nullptr};

  // Declared last: torn down first, so no build outlives the rest.
  runtime::RuntimeHost runtime;

  // ---- sessions ----
  Writer &WriterOf(Session session) {
    std::unique_lock<std::mutex> lock(mutex);
    auto it = writers.find(session);
    if (it == writers.end()) {
      changed.wait(lock, [&] { return opening == 0; });
      it = writers.emplace(session, std::make_unique<Writer>()).first;
    }
    return *it->second;
  }

  Writer *FindWriter(Session session) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = writers.find(session);
    return it == writers.end() ? nullptr : it->second.get();
  }

  void EndTransaction(Session session) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      writers.erase(session);
    }
    changed.notify_all();
  }

  // The columns to capture of `table` (nullopt: no accelerator watches it).
  const std::optional<std::vector<int32_t>> &Watched(Writer &writer, core::TableId table) {
    if (writer.watched_version != runtime.CaptureVersion()) {
      writer.watched.clear();
      writer.watched_version = runtime.CaptureVersion();
    }
    auto it = writer.watched.find(table);
    if (it == writer.watched.end()) {
      it = writer.watched.emplace(table, runtime.CaptureColumns(table)).first;
    }
    return it->second;
  }

  // The statement's rows join the transaction (a failed statement's are dropped).
  void EndStatement(Session session, Writer &writer, bool failed) {
    auto batches = writer.statement.Take();
    if (failed) {
      return;
    }
    for (auto &batch : batches) {
      runtime.OnCapture(session, 0, std::move(batch));
    }
  }

  void Commit(Session session, Writer &writer) {
    if (writer.poisoned) {
      Abort(session);
      InvalidateAll();
      return;
    }
    try {
      runtime.PreCommit(session);
      // Taken while the commit holds its accelerators' places in commit
      // order: epochs grow with each accelerator's commit order.
      runtime.Commit(session, ++epoch);
    } catch (...) {
      runtime.Abort(session);
      InvalidateAll();
    }
    EndTransaction(session);
  }

  void Abort(Session session) {
    runtime.Abort(session);
    EndTransaction(session);
  }

  void InvalidateAll() {
    for (auto *accelerator : runtime.All()) {
      accelerator->Invalidate();
    }
  }

  // ---- slots ----
  // Readers always read the newest commit, so every one waits for an
  // in-flight commit.
  Epoch PendingEpochLowerBound(uint64_t) const override { return 0; }
  Epoch OldestReaderEpoch() const override { return kNoFloor; }
  // Every session that wrote since before the registration is a writer, and
  // OpenBuildSnapshot waits for all writers.
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) override {
    return true;
  }
  std::unique_ptr<runtime::BuildSnapshot> OpenBuildSnapshot(runtime::AcceleratorRuntime &accelerator) override {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, BuildWait(), [&] { return writers.empty(); })) {
      throw std::runtime_error("writers did not finish before the build");
    }
    // Every commit up to now is in the view and has an epoch <= `at`; no
    // session writes until the view is open, so every later commit is after it.
    const Epoch at = epoch;
    opening++;
    lock.unlock();
    struct Reopen {
      Impl &impl;
      ~Reopen() {
        {
          std::lock_guard<std::mutex> lock(impl.mutex);
          impl.opening--;
        }
        impl.changed.notify_all();
      }
    } reopen{*this};
    return std::make_unique<MysqlBuildSnapshot>(at, std::min(accelerator.WorkerCount(), MaxScanLanes()));
  }
  bool EngineWorkOnHostThread() const override { return false; }
  void RunBuild(runtime::AcceleratorRuntime &accelerator, runtime::BuildSnapshot &,
                core::StateBuilder &builder) override {
    const detail::ScanServicesScope scans(accelerator);
    accelerator.Module().Build(accelerator, builder);
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &id) const override {
    std::lock_guard<std::mutex> lock(mutex);
    return cost_models.at(id);
  }
  uint64_t TableRows(core::TableId table) const override { return table_rows.Rows(table); }
  double ExpectedQueriesBeforeNextWrite(const core::AcceleratorId &) const override {
    const double override_value = core::ExpectedQueriesOverride();
    return override_value >= 0.0 ? override_value : 10.0;
  }
};

MysqlHost &MysqlHost::Get() {
  static MysqlHost host;
  return host;
}

MysqlHost::MysqlHost() : impl(std::make_unique<Impl>()) {}

MysqlHost::~MysqlHost() = default;

runtime::AcceleratorRuntime &MysqlHost::Register(std::shared_ptr<core::AuxQueryModule> module,
                                                 maintenance::CostModelParams cost_model,
                                                 const runtime::TableMap &tables) {
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->cost_models[module->Id()] = std::move(cost_model);
  }
  return impl->runtime.Register(std::move(module), nullptr, tables);
}

runtime::RuntimeHost &MysqlHost::Runtime() { return impl->runtime; }

void MysqlHost::SetThreads(const ThreadSettings &settings) { impl->SetThreads(settings); }

// ---- engine callbacks ----

void MysqlHost::OnCapturedRow(Session session, const CapturedRow &row) {
  impl->captured_rows++;
  auto &writer = impl->WriterOf(session);
  if (const auto &columns = impl->Watched(writer, row.table)) {
    writer.statement.Add(row, *columns);
  }
}

void MysqlHost::OnCaptureFailure(Session session) {
  impl->WriterOf(session); // the transaction's end is awaited by the rebuild
  InvalidateAll();
}

void MysqlHost::OnStatementEnd(Session session, MysqlStatementKind kind, bool failed,
                               bool in_active_transaction) {
  auto *writer = impl->FindWriter(session);
  if (!writer) {
    return; // this session captured nothing
  }
  switch (kind) {
  case MysqlStatementKind::Commit:
    if (failed) { // the commit's outcome is unknown
      impl->Abort(session);
      InvalidateAll();
    } else {
      impl->EndStatement(session, *writer, false);
      impl->Commit(session, *writer);
    }
    break;
  case MysqlStatementKind::Rollback:
    impl->Abort(session);
    break;
  case MysqlStatementKind::RollbackToSavepoint:
    impl->EndStatement(session, *writer, false);
    writer->poisoned = writer->poisoned || !failed;
    break;
  case MysqlStatementKind::Begin: // commits a transaction still open
    impl->EndStatement(session, *writer, failed);
    impl->Commit(session, *writer);
    break;
  case MysqlStatementKind::Other:
    impl->EndStatement(session, *writer, failed);
    if (!in_active_transaction) {
      if (failed) {
        impl->Abort(session);
      } else {
        impl->Commit(session, *writer);
      }
    }
    break;
  }
}

bool MysqlHost::SessionWroteDependency(Session session, const runtime::AcceleratorRuntime &accelerator) {
  const auto *writer = impl->FindWriter(session);
  return writer && (writer->poisoned || impl->runtime.TxnWroteDependency(session, accelerator));
}

void MysqlHost::OnSessionEnd(Session session) {
  if (impl->FindWriter(session)) {
    impl->Abort(session);
  }
}

void MysqlHost::InvalidateAll() { impl->InvalidateAll(); }

// ---- diagnostics ----

uint64_t MysqlHost::CapturedRowCount() const { return impl->captured_rows; }

void MysqlHost::ResetCapturedRowCount() { impl->captured_rows = 0; }

void MysqlHost::NoteRead(const runtime::AcceleratorRuntime &accelerator) { impl->last_read = &accelerator; }

std::string MysqlHost::LastMaintenanceAction() const {
  const auto *accelerator = impl->last_read.load();
  const auto decision = accelerator ? accelerator->Stats().last_decision : std::nullopt;
  return decision && decision->valid ? maintenance::MaintenanceActionName(decision->staged_action) : "";
}

std::string MysqlHost::LastMaintenanceDecision() const {
  const auto *accelerator = impl->last_read.load();
  const auto decision = accelerator ? accelerator->Stats().last_decision : std::nullopt;
  if (!decision) {
    return "";
  }
  core::SetGlobalMaintenanceDecision(*decision);
  return core::FormatLastMaintenanceDecision();
}

void MysqlHost::ForceMaintenanceAction(const std::string &action) {
  core::ClearMaintenanceDecisionLog();
  core::SetForcedMaintenanceActionName(action);
}

std::size_t MysqlHost::WriterCount() const {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return impl->writers.size();
}

runtime::AcceleratorStats MysqlHost::TotalStats() const {
  runtime::AcceleratorStats total;
  for (auto *accelerator : impl->runtime.All()) {
    const auto stats = accelerator->Stats();
    total.commits_applied += stats.commits_applied;
    total.rows_applied += stats.rows_applied;
    total.apply_us += stats.apply_us;
    total.builds += stats.builds;
    total.build_us += stats.build_us;
    total.catch_up_commits += stats.catch_up_commits;
    total.reads_hit += stats.reads_hit;
    total.reads_waited += stats.reads_waited;
    total.reads_fallback += stats.reads_fallback;
  }
  return total;
}

} // namespace accelerator_codegen::aux_state::engine::mysql

#endif
