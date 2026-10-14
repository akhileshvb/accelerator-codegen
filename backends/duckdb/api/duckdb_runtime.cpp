// GENERATED BACKEND
#include "backends/duckdb/api/duckdb_runtime.hpp"
#include "backends/duckdb/api/verus_host.hpp"
#include "backends/duckdb/api/shared_query_engine.hpp"
#include "duckdb/common/types/value.hpp"
#include <chrono>
#include <atomic>
#include <cctype>
#include "duckdb/main/attached_database.hpp"
#include <map>
#include "duckdb/accelerator/mutation_recorder.hpp"
#include "duckdb/accelerator/snapshot_override.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <cmath>
#include <limits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <exception>
#include <thread>
#include <vector>
#include <shared_mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/engine/capture.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_maintenance_policy.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "eval/harness/aux_state_footprint.hpp"
#include "eval/harness/cost_calibration.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

namespace core = accelerator_codegen::aux_state::core;
namespace duckdb_backend = accelerator_codegen::aux_state::engine::duckdb;

} // namespace

// ---- the aux query registry (aux_query_registry.hpp) ----

namespace {

std::vector<std::unique_ptr<AuxQuery>> &AuxQueryStore() {
  static std::vector<std::unique_ptr<AuxQuery>> store;
  return store;
}

} // namespace

void RegisterAuxQuery(AuxQuery query) {
  AuxQueryStore().push_back(std::make_unique<AuxQuery>(std::move(query)));
}

std::vector<const AuxQuery *> AuxQueries() {
  std::vector<const AuxQuery *> out;
  for (const auto &query : AuxQueryStore()) {
    out.push_back(query.get());
  }
  return out;
}

const AuxQuery *FindAuxQuery(const std::string &accelerator_id) {
  for (const auto &query : AuxQueryStore()) {
    if (query->accelerator_id == accelerator_id) {
      return query.get();
    }
  }
  return nullptr;
}

namespace {

namespace core = accelerator_codegen::aux_state::core;
using namespace scan;

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

struct Builder final : core::StateBuilder {
  void Write(core::StateImage image) override {
    images.push_back(std::move(image));
  }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

std::unique_ptr<DuckdbRuntime::Binding>
CreateBinding(::duckdb::Connection &connection,
              const SharedQueryCatalogEntry &entry) {
  Require(entry.accelerator_id != nullptr, "catalog entry without an accelerator id");
  if (auto kit = MakeVerusKitBinding(connection, entry.accelerator_id)) {
    return kit;
  }
  const auto *query = FindAuxQuery(entry.accelerator_id);
  Require(query != nullptr && static_cast<bool>(query->make_binding),
          std::string("no aux query registered for accelerator ") + entry.accelerator_id);
  return query->make_binding(connection);
}

// Active runtimes: a handful (one per registered accelerator), read on every
// query and written on build/teardown. A vector with a shared lock: a lookup
// allocates nothing and concurrent readers do not serialize.
struct ActiveRuntimeEntry {
  ::duckdb::DatabaseInstance *db = nullptr;
  std::string accelerator_id;
  DuckdbRuntime *runtime = nullptr;
};

std::shared_mutex &ActiveRuntimeMutex() {
  static std::shared_mutex mutex;
  return mutex;
}

std::vector<ActiveRuntimeEntry> &ActiveRuntimes() {
  static std::vector<ActiveRuntimeEntry> runtimes;
  return runtimes;
}

void RegisterActiveRuntime(DuckdbRuntime *runtime,
                           ::duckdb::Connection &connection,
                           const SharedQueryCatalogEntry &entry) {
  if (!runtime || !entry.accelerator_id || !connection.context ||
      !connection.context->db) {
    return;
  }
  auto *db = connection.context->db.get();
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (auto &active : ActiveRuntimes()) {
    if (active.db == db && active.accelerator_id == entry.accelerator_id) {
      active.runtime = runtime;
      return;
    }
  }
  ActiveRuntimes().push_back({db, entry.accelerator_id, runtime});
}

void UnregisterActiveRuntime(DuckdbRuntime *runtime) {
  if (!runtime) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  auto &runtimes = ActiveRuntimes();
  runtimes.erase(std::remove_if(runtimes.begin(), runtimes.end(),
                                [&](const ActiveRuntimeEntry &e) { return e.runtime == runtime; }),
                 runtimes.end());
}

DuckdbRuntime *FindActiveRuntimeImpl(::duckdb::DatabaseInstance &db,
                                     const std::string &accelerator_id) {
  std::shared_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (const auto &active : ActiveRuntimes()) {
    if (active.db == &db && active.accelerator_id == accelerator_id) {
      return active.runtime;
    }
  }
  return nullptr;
}

} // namespace

DuckdbRuntime *DuckdbRuntime::FindActive(::duckdb::DatabaseInstance &db,
                                         const std::string &accelerator_id) {
  return FindActiveRuntimeImpl(db, accelerator_id);
}

namespace {

// The oldest start time of an open transaction: aux versions older than the
// one it selects can be pruned.
DuckDBStateStore::FloorProvider OldestSnapshotFloor(::duckdb::ClientContext &context) {
  auto db = AuxCommitClock::DefaultDatabase(context);
  if (!db) {
    return {};
  }
  return [db]() -> core::Version {
    auto &manager = db->GetTransactionManager();
    if (!manager.IsDuckTransactionManager()) {
      return std::numeric_limits<core::Version>::max();
    }
    return manager.Cast<::duckdb::DuckTransactionManager>().LowestActiveStart();
  };
}

// The snapshot epoch from which a state built now is current: the start
// time of a transaction begun now (every commit so far is in it; no captured
// commit runs during the build, since commits go through this connection).
// GetLastCommit() is not on the start-time scale for a database opened from a
// file, so it cannot be used here.
core::Version BuildEpoch(::duckdb::Connection &connection) {
  auto scan_lock = LockScan(connection);
  if (connection.HasActiveTransaction()) {
    return DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  }
  connection.BeginTransaction();
  const auto epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  connection.Rollback();
  return epoch;
}

} // namespace


void DuckdbRuntime::EnableMutationCapture() {
  if (capture_enabled_) {
    return;
  }
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Add(table.first, dep.mutation_attnums);
      }
    }
  }
  connection_.context->registered_state->GetOrCreate<AuxCaptureHub>(AuxCaptureHub::kKey)->Add(this);
  capture_enabled_ = true;
}

void DuckdbRuntime::DisableMutationCapture() {
  if (!capture_enabled_) {
    return;
  }
  if (auto hub = connection_.context->registered_state->Get<AuxCaptureHub>(AuxCaptureHub::kKey)) {
    if (hub->Remove(this)) {
      connection_.context->registered_state->Remove(AuxCaptureHub::kKey);
    }
  }
  auto &watched = WatchedColumns(*connection_.context);
  const auto tables = binding_->CaptureTables();
  for (const auto &dep : binding_->Module().Dependencies()) {
    for (const auto &table : tables) {
      if (table.second == dep.table) {
        watched.Remove(table.first, dep.mutation_attnums);
      }
    }
  }
  capture_enabled_ = false;
}

void DuckdbRuntime::OnCapturedEvent(const CapturedEvent &event) {
  constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  core::TableId table_id = 0;
  bool known = false;
  for (const auto &table : binding_->CaptureTables()) {
    if (table.first == event.table_name) {
      table_id = table.second;
      known = true;
    }
  }
  if (!known) {
    return;
  }
  const bool is_update = event.kind == core::MutationKind::Update;
  std::vector<std::size_t> old_pos, new_pos;
  for (const auto &dep : binding_->Module().Dependencies()) {
    if (dep.table != table_id || dep.mutation_attnums.empty()) {
      continue;
    }
    const std::size_t n = dep.mutation_attnums.size();
    core::MutationBatch batch;
    batch.table = table_id;
    batch.kind = event.kind;
    if (is_update) {
      // Delivered even when no captured column changed (see TryGetUpdateOldColumns):
      // the old and new projections then are equal -- a key-only delta's "touched".
      for (std::size_t j = 0; j < n; ++j) {
        for (auto u : event.updated_columns) {
          if (u == dep.mutation_attnums[j]) {
            batch.updated_columns.push_back(static_cast<core::ColumnId>(j));
          }
        }
      }
    }
    // Position of each projected column among the event's values: the old
    // values first (a later duplicate wins), then the updated columns' new
    // values; the new row is the old one with those replaced.
    old_pos.assign(n, kNone);
    new_pos.assign(n, kNone);
    for (std::size_t j = 0; j < n; ++j) {
      for (std::size_t i = 0; i < event.width; ++i) {
        if (event.columns[i] == dep.mutation_attnums[j]) {
          (is_update && i >= event.olds ? new_pos : old_pos)[j] = i;
        }
      }
    }
    auto project = [&](std::size_t r, bool use_new) {
      core::AuxRow out;
      out.values.reserve(n);
      const core::AuxValue *base = event.values.data() + r * event.width;
      for (std::size_t j = 0; j < n; ++j) {
        std::size_t pos = use_new ? new_pos[j] : kNone;
        if (pos == kNone) {
          pos = old_pos[j];
        }
        out.values.push_back(pos == kNone ? core::AuxValue::Null(core::AuxType::String) : base[pos]);
      }
      return out;
    };
    switch (event.kind) {
    case core::MutationKind::Insert:
      batch.new_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.new_rows.push_back(project(r, false));
      }
      break;
    case core::MutationKind::Delete:
      batch.old_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.old_rows.push_back(project(r, false));
      }
      break;
    case core::MutationKind::Update:
      batch.old_rows.reserve(event.rows);
      batch.new_rows.reserve(event.rows);
      for (std::size_t r = 0; r < event.rows; ++r) {
        batch.old_rows.push_back(project(r, false));
        batch.new_rows.push_back(project(r, true));
      }
      break;
    }
    if (!batch.old_rows.empty() || !batch.new_rows.empty()) {
      backend_.CaptureMutation(batch);
    }
  }
}

void DuckdbRuntime::BeginWrite() { connection_.BeginTransaction(); }

uint64_t DuckdbRuntime::ApplyWrite(const std::string &sql) {
  BeginWrite();
  // statement by statement (one transaction), so each INSERT / UPDATE / DELETE reports its row count
  uint64_t rows = 0;
  try {
    for (auto &statement : connection_.ExtractStatements(sql)) {
      auto result = connection_.Query(std::move(statement));
      if (!result || result->HasError()) {
        throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
      }
      if (result->ColumnCount() == 1 && result->RowCount() == 1 && result->names[0] == "Count") {
        rows += static_cast<uint64_t>(result->GetValue(0, 0).GetValue<int64_t>());
      }
    }
  } catch (...) {
    RollbackWrite();
    throw;
  }
  CommitWrite();
  return rows;
}

void DuckdbRuntime::ApplyWriteAll(const std::vector<DuckdbRuntime *> &runtimes, const std::string &sql,
                                  WriteAllTiming *timing) {
  if (runtimes.empty())
    throw std::invalid_argument("ApplyWriteAll: no runtimes");
  auto &connection = runtimes.front()->connection_;
  for (auto *rt : runtimes)
    if (&rt->connection_ != &connection)
      throw std::invalid_argument("ApplyWriteAll: runtimes on different connections");
  const auto abort_all = [&] {
    for (auto *rt : runtimes)
      rt->coordinator_.Abort();
    try {
      if (connection.HasActiveTransaction())
        connection.Rollback();
    } catch (...) {
    }
  };
  using TimingClock = std::chrono::steady_clock;
  const auto us = [](TimingClock::time_point a, TimingClock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
  };
  if (timing)
    *timing = WriteAllTiming{};
  auto t0 = TimingClock::now();
  connection.BeginTransaction();
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    abort_all();
    throw std::runtime_error(sql + ": " + (result ? result->GetError() : "null result"));
  }
  if (timing)
    timing->statement_us = us(t0, TimingClock::now());
  // as CommitWrite, for every runtime: one clock (per database), each
  // coordinator's PRE_COMMIT, the engine commit, each one's COMMIT
  runtimes.front()->clock_->epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context) + 1;
  try {
    // PRE_COMMIT of every runtime. Each coordinator, backend, module and
    // state store is its own; what they share is the writer's connection
    // (every scan serializes on it, see scan::LockScan), the Verus world
    // store (internally locked; WorldShared::Begin counts participants under
    // its mutex) and the decision log (thread-local per runtime here).
    // ACCEL_PRECOMMIT_THREADS=1 is the serial loop.
    std::size_t threads = std::min<std::size_t>(runtimes.size(), std::max(1u, std::thread::hardware_concurrency()));
    if (const char *env = std::getenv("ACCEL_PRECOMMIT_THREADS"); env && *env) {
      const long parsed = std::strtol(env, nullptr, 10);
      if (parsed > 0)
        threads = std::min<std::size_t>(runtimes.size(), static_cast<std::size_t>(parsed));
    }
    if (threads <= 1) {
      for (auto *rt : runtimes) {
        if (timing)
          core::ClearMaintenanceDecisionLog();
        t0 = TimingClock::now();
        rt->coordinator_.PreCommit();
        if (timing) {
          timing->pre_commit_us.push_back(us(t0, TimingClock::now()));
          const auto d = core::LastMaintenanceDecision();
          timing->action.push_back(d ? maintenance::MaintenanceActionName(d->staged_action) : "none");
          timing->decision.push_back(d ? *d : core::MaintenanceDecisionLogEntry{});
        }
      }
    } else {
      const std::size_t n = runtimes.size();
      std::vector<double> pre_us(n, 0.0);
      std::vector<std::optional<core::MaintenanceDecisionLogEntry>> decisions(n);
      std::vector<std::exception_ptr> errors(n);
      std::atomic<std::size_t> next{0};
      std::atomic<bool> failed{false};
      const auto work = [&] {
        for (;;) {
          const std::size_t i = next.fetch_add(1);
          if (i >= n || failed.load())
            return;
          try {
            core::ScopedThreadMaintenanceDecisionLog scoped_log;
            const auto start = TimingClock::now();
            runtimes[i]->coordinator_.PreCommit();
            pre_us[i] = us(start, TimingClock::now());
            decisions[i] = core::LastMaintenanceDecision();
          } catch (...) {
            errors[i] = std::current_exception();
            failed = true;
            return;
          }
        }
      };
      std::vector<std::thread> pool;
      pool.reserve(threads - 1);
      try {
        for (std::size_t k = 1; k < threads; ++k)
          pool.emplace_back(work);
      } catch (...) {
        failed = true; // could not start a thread: let the started ones finish, then abort
        for (auto &t : pool)
          t.join();
        throw;
      }
      work();
      for (auto &t : pool)
        t.join();
      for (auto &e : errors)
        if (e)
          std::rethrow_exception(e);
      if (timing) {
        for (std::size_t i = 0; i < n; ++i) {
          const auto &d = decisions[i];
          timing->pre_commit_us.push_back(pre_us[i]);
          timing->action.push_back(d ? maintenance::MaintenanceActionName(d->staged_action) : "none");
          timing->decision.push_back(d ? *d : core::MaintenanceDecisionLogEntry{});
        }
      }
      // what the serial loop leaves in the process-wide log: the last runtime's
      core::ClearMaintenanceDecisionLog();
      if (decisions[n - 1])
        core::SetGlobalMaintenanceDecision(*decisions[n - 1]);
    }
    t0 = TimingClock::now();
    connection.Commit();
    if (timing)
      timing->engine_commit_us = us(t0, TimingClock::now());
  } catch (...) {
    abort_all();
    throw;
  }
  // COMMIT (install) of every runtime. The engine commit is already visible,
  // so every runtime must run its install even if another throws: a skipped
  // one would stay "pending" and block newest-snapshot readers forever. Each
  // install touches only its own coordinator/store (FinishCommit appends one
  // version to that accelerator's list and clears its pending flag); all
  // installs publish at the same, already fixed, commit epoch, so install order
  // is not observable. The Verus world store assembles its batch under its own
  // mutex (last finisher installs). ACCEL_COMMIT_THREADS=1 is the serial loop.
  const std::size_t n = runtimes.size();
  std::size_t threads = std::min<std::size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
  for (const char *name : {"ACCEL_COMMIT_THREADS", "ACCEL_PRECOMMIT_THREADS"}) {
    if (const char *env = std::getenv(name); env && *env) {
      const long parsed = std::strtol(env, nullptr, 10);
      if (parsed > 0) {
        threads = std::min<std::size_t>(n, static_cast<std::size_t>(parsed));
        break;
      }
    }
  }
  if (threads <= 1) {
    for (auto *rt : runtimes) {
      t0 = TimingClock::now();
      rt->coordinator_.Commit();
      if (timing)
        timing->commit_us.push_back(us(t0, TimingClock::now()));
    }
    return;
  }
  std::vector<double> commit_us(n, 0.0), maint_us(n, 0.0);
  std::vector<std::exception_ptr> errors(n);
  std::atomic<std::size_t> next{0};
  const auto work = [&] {
    for (;;) {
      const std::size_t i = next.fetch_add(1);
      if (i >= n)
        return;
      try {
        core::ScopedThreadMaintenanceDecisionLog scoped_log;
        const auto start = TimingClock::now();
        runtimes[i]->coordinator_.Commit();
        commit_us[i] = us(start, TimingClock::now());
        if (const auto d = core::LastMaintenanceDecision())
          maint_us[i] = d->maintenance_us;
      } catch (...) {
        errors[i] = std::current_exception();
      }
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  try {
    for (std::size_t k = 1; k < threads; ++k)
      pool.emplace_back(work);
  } catch (...) {
    // could not start a thread: the ones started plus this thread still drain all work
  }
  work();
  for (auto &t : pool)
    t.join();
  if (timing)
    timing->commit_us.insert(timing->commit_us.end(), commit_us.begin(), commit_us.end());
  // the process-wide log keeps what the serial loop leaves: the last staged runtime's
  for (std::size_t i = n; i-- > 0;)
    if (maint_us[i] > 0.0) {
      core::RecordCoordinatorMaintenanceUs(maint_us[i]);
      break;
    }
  for (auto &e : errors)
    if (e)
      std::rethrow_exception(e);
}

std::vector<core::CalibrationWrite> DuckdbRuntime::CalibrationWrites() const {
  return binding_->Module().CalibrationWrites();
}

core::CalibrationEngine::Timed
DuckdbCalibrationEngine::WriteThenRead(const std::string &sql,
                                       std::optional<maintenance::MaintenanceAction> forced) {
  core::SetForcedMaintenanceActionName(forced ? maintenance::MaintenanceActionName(*forced) : "");
  core::ClearMaintenanceDecisionLog();
  Timed timed;
  const auto start = std::chrono::steady_clock::now();
  try {
    runtime.ApplyWrite(sql);
    const auto read_start = std::chrono::steady_clock::now();
    runtime.Execute();
    timed.read_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                              read_start)
                        .count();
  } catch (...) {
    core::SetForcedMaintenanceActionName("");
    throw;
  }
  timed.us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  core::SetForcedMaintenanceActionName("");
  timed.decision = core::LastMaintenanceDecision();
  return timed;
}

void DuckdbRuntime::RollbackWrite() {
  connection_.Rollback();
  coordinator_.Abort();
}

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection,
                             const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry),
      clock_(connection.context->registered_state->GetOrCreate<AuxCommitClock>(
          "accel_aux_commit_clock", AuxCommitClock::DefaultDatabase(*connection.context))),
      state_store_(entry.accelerator_id,
                   [clock = clock_] { return clock->epoch.load(); },
                   OldestSnapshotFloor(*connection.context)),
      // Same epochs and floor as state_store_: the two stores differ only
      // in their protocol's implementation.
      verus_store_(MakeRequestedVerusStore(entry.accelerator_id,
                                           [clock = clock_] { return clock->epoch.load(); },
                                           OldestSnapshotFloor(*connection.context))),
      coordinator_(),
      expected_queries_before_write_(
          std::make_shared<double>(expected_queries_before_write)),
      backend_(coordinator_, ActiveStore(),
               [](const ::duckdb::TableMutationEvent &) {
                 return core::MutationBatch{};
               },
               [] { return DuckDBAuxBackend::ReaderVisibility(); },
               [entry] {
                 return core::LoadAuxCostModelJson(
                     harness::SharedCostModelConfigPath(entry));
               },
               [expected = expected_queries_before_write_]() {
                 return *expected;
               }),
      binding_(CreateBinding(connection, entry)) {
  coordinator_.Register(std::shared_ptr<core::AuxQueryModule>(
      &binding_->Module(), [](core::AuxQueryModule *) {}));
  backend_.SetExecutionContext(connection.context.get());
}

void DuckdbRuntime::CommitWrite() {
  // Readers whose transaction starts after this writer's may see the
  // commit: they wait for its publish. (The commit hook then moves the
  // epoch to the commit id + 1 for the publish.)
  clock_->epoch = DuckDBAuxBackend::SnapshotEpoch(*connection_.context) + 1;
  try {
    coordinator_.PreCommit();
    connection_.Commit();
  } catch (...) {
    coordinator_.Abort();
    // leave no open (or aborted) engine transaction behind: the next write starts clean
    try {
      if (connection_.HasActiveTransaction())
        connection_.Rollback();
    } catch (...) {
    }
    throw;
  }
  coordinator_.Commit();
}

void DuckdbRuntime::SetExpectedQueriesBeforeNextWrite(
    double expected_queries_before_write) {
  *expected_queries_before_write_ = expected_queries_before_write;
}

double DuckdbRuntime::ExpectedQueriesBeforeNextWrite() const {
  return *expected_queries_before_write_;
}

void DuckdbRuntime::Build(bool register_active) {
  const auto build_epoch = BuildEpoch(connection_);
  Builder builder;
  binding_->Build(backend_, builder);
  Require(builder.finished, "shared DuckDB build did not finish");
  // Current from the snapshot the build read (no captured commit can run
  // meanwhile: commits go through this runtime's connection).
  if (verus_store_) {
    verus_store_->Install(std::move(builder.images), entry_.accelerator_id,
                          build_epoch);
  } else {
    state_store_.Install(std::move(builder.images), entry_.accelerator_id,
                         build_epoch);
  }
  if (register_active) {
    RegisterActiveRuntime(this, connection_, entry_);
    // Readable now: fill the table function's schema cache so the first read
    // doesn't plan the base SQL.
    ::accelerator_codegen::aux_state::engine::PrimeAuxSchema(connection_, entry_);
  }
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

void DuckdbRuntime::ExecuteInto(const core::QueryArguments &arguments, core::AuxRowSink &sink) {
  AuxReadScope one_version; // every read in this query: one state version
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks,
                     static_cast<::duckdb::transaction_t>(DuckDBAuxBackend::SnapshotEpoch(*reader)));
  }
  t_table_scans = 0;
  binding_->Execute(backend_, arguments, sink);
  core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                           : core::AuxExecutePath::Hit);
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(
    const core::QueryArguments &arguments) {
  AuxReadScope one_version; // every read in this query: one state version
  // Normally unused: commits publish valid state (ReadersCanBuildState).
  std::optional<ScanRedirect> redirect;
  auto *reader = DuckDBAuxBackend::CurrentReader();
  if (reader && reader != connection_.context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks,
                     static_cast<::duckdb::transaction_t>(DuckDBAuxBackend::SnapshotEpoch(*reader)));
  }
  MaterializeSink sink;
  t_table_scans = 0;
  binding_->Execute(backend_, arguments, sink);
  core::RecordAuxExecute(t_table_scans > 0 ? core::AuxExecutePath::Fallback
                                           : core::AuxExecutePath::Hit);
  last_rows = sink.rows.size();
  return std::move(sink.rows);
}

void DuckdbRuntime::Execute() {
  Execute(ToCoreArguments(entry_.benchmark_arguments));
}

void DuckdbRuntime::Execute(const core::QueryArguments &arguments) {
  core::BeginAuxExecuteProfiling();
  auto rows = ExecuteCollect(arguments);
  core::EndAuxExecuteProfiling();
  last_rows = rows.size();
}

namespace {

// Common envelope of a timed maintenance write: fresh decision log, wall time
// of `write` (begin ... commit) recorded as the actual maintenance cost.
template <typename Write> void TimedMaintenanceWrite(Write &&write) {
  core::ClearMaintenanceDecisionLog();
  const auto start = std::chrono::steady_clock::now();
  write();
  const auto end = std::chrono::steady_clock::now();
  core::RecordMaintenanceActualCostUs(static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));
}

// Runs one write statement inside the open transaction; rolls back and throws on error.
void RunWriteQuery(::duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  if (result == nullptr || result->HasError()) {
    const auto error = result ? result->GetError() : "DuckDB write failed";
    connection.Rollback();
    Require(false, error);
  }
}

} // namespace

void DuckdbRuntime::Apply(SharedProbe &probe) {
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    RunWriteQuery(connection_, ProbeWriteSql(probe));
    backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
    CommitWrite();
    AdvanceProbe(probe);
  });
}

void DuckdbRuntime::ApplyBatch(std::vector<SharedProbe> &probes) {
  Require(!probes.empty(), "ApplyBatch requires at least one probe");
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    for (auto &probe : probes) {
      RunWriteQuery(connection_, ProbeWriteSql(probe));
      backend_.CaptureMutation(BuildSharedMutation(entry_, probe));
      AdvanceProbe(probe);
    }
    CommitWrite();
  });
}

void DuckdbRuntime::ApplyBatchWrite(const std::string &sql,
                                    const core::MutationBatch &mutation) {
  TimedMaintenanceWrite([&] {
    connection_.BeginTransaction();
    RunWriteQuery(connection_, sql);
    backend_.CaptureMutation(mutation);
    CommitWrite();
  });
}

void DuckdbRuntime::ResetCalibrationState() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
  coordinator_.Abort();
}

void DuckdbRuntime::InvalidateAll() {
  ActiveStore().Invalidate(core::AllPartitionsPlan());
}

core::AuxStateStore &DuckdbRuntime::ActiveStore() {
  if (verus_store_) {
    return *verus_store_;
  }
  return state_store_;
}

std::size_t DuckdbRuntime::StateVersionCount() const {
  return verus_store_ ? verus_store_->TotalVersionCount()
                      : state_store_.TotalVersionCount();
}

std::size_t DuckdbRuntime::StaleInstallCount() const {
  return verus_store_ ? verus_store_->StaleInstalls() : 0;
}

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  // No engine transaction: publish for every snapshot (epoch 0).
  clock_->epoch = 0;
  backend_.CaptureMutation(mutation);
  coordinator_.PreCommit();
  coordinator_.Commit();
}

void DuckdbRuntime::ExecuteBaseQuery() {
  // A Verus kit's SQL is parameterized ($1, ...): its own fallback runs it
  // with the benchmark arguments.
  if (IsVerusKitAccelerator(entry_.accelerator_id)) {
    MaterializeSink sink;
    binding_->Module().ExecuteFallback(backend_, ToCoreArguments(entry_.benchmark_arguments), sink);
    return;
  }
  auto result = connection_.Query(entry_.base_sql);
  Require(result != nullptr && !result->HasError(),
          result ? result->GetError() : "DuckDB base query failed");
  for (::duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (::duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
}

core::AuxStoreDiagnosticsSnapshot DuckdbRuntime::StoreDiagnosticsSnapshot() const {
  if (verus_store_) {
    return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
        static_cast<uint64_t>(verus_store_->ValidPartitionCount()),
        static_cast<uint64_t>(verus_store_->TotalPartitionCount()));
  }
  return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(
      static_cast<uint64_t>(state_store_.ValidPartitionCount()),
      static_cast<uint64_t>(state_store_.TotalPartitionCount()));
}

void DuckdbRuntime::ResetStoreDiagnosticsProfiling() {
  core::ResetAuxStoreDiagnosticsProfiling();
}

uint64_t DuckdbRuntime::AuxStateFootprintBytes() const {
  if (verus_store_) {
    return 0; // the Verus store's states are not measured
  }
  return harness::ComputeAuxStateFootprintBytes(state_store_,
                                                entry_.accelerator_id);
}

DuckdbRuntime::~DuckdbRuntime() {
  UnregisterActiveRuntime(this);
  DisableMutationCapture();
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
