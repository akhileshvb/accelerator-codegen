// GENERATED BACKEND
#include "backends/duckdb/api/duckdb_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <numeric>
#include <shared_mutex>
#include <stdexcept>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/duckdb_host.hpp"
#include "backends/duckdb/api/shared_query_engine.hpp"
#include "backends/duckdb/api/verus_host.hpp"
#include "backends/duckdb/api/scan.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/state/aux_state_footprint.hpp"
#include "substrate/aux/state/verus_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

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

using Clock = std::chrono::steady_clock;

double MicrosSince(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// The ceiling on a reader's wait for an in-flight commit or a build; the
// runtime falls back sooner when the SQL is cheaper than waiting (C11).
uint64_t ReadWaitUs() {
  static const uint64_t wait = [] {
    const char *value = std::getenv("ACCEL_AUX_READ_WAIT_US");
    return value && *value ? std::strtoull(value, nullptr, 10) : 600'000'000ULL;
  }();
  return wait;
}

struct MaterializeSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { rows.push_back(row); }
  void End() override {}
  std::vector<core::AuxRow> rows;
};

// Active runtimes, read on every aux table-function query.
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

void RegisterActiveRuntime(DuckdbRuntime *runtime, ::duckdb::DatabaseInstance &db,
                           const std::string &accelerator_id) {
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (auto &active : ActiveRuntimes()) {
    if (active.db == &db && active.accelerator_id == accelerator_id) {
      active.runtime = runtime;
      return;
    }
  }
  ActiveRuntimes().push_back({&db, accelerator_id, runtime});
}

void UnregisterActiveRuntime(DuckdbRuntime *runtime) {
  std::unique_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  auto &runtimes = ActiveRuntimes();
  runtimes.erase(std::remove_if(runtimes.begin(), runtimes.end(),
                                [&](const ActiveRuntimeEntry &e) { return e.runtime == runtime; }),
                 runtimes.end());
}

// Runs one statement of the open write transaction; rolls back and throws on error.
void RunWriteQuery(::duckdb::Connection &connection, const std::string &sql) {
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    const auto error = result ? result->GetError() : "DuckDB write failed";
    connection.Rollback();
    throw std::runtime_error(error);
  }
}

// The epoch of a transaction begun now on `connection` (its own, if open).
runtime::Epoch CurrentEpoch(::duckdb::Connection &connection) {
  if (connection.HasActiveTransaction()) {
    return DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  }
  connection.BeginTransaction();
  const auto epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  connection.Rollback();
  return epoch;
}

// A timed maintenance write: fresh decision log, wall time of `write` recorded
// as the actual maintenance cost.
template <typename Write> void TimedMaintenanceWrite(Write &&write) {
  core::ClearMaintenanceDecisionLog();
  const auto start = Clock::now();
  write();
  core::RecordMaintenanceActualCostUs(MicrosSince(start));
}

} // namespace

DuckdbRuntime::DuckdbRuntime(::duckdb::Connection &connection, const SharedQueryCatalogEntry &entry,
                             double expected_queries_before_write)
    : connection_(connection), entry_(entry), host_(DuckdbHost::Get(*connection.context->db)),
      accelerator_(host_->Ensure(entry)) {
  SetExpectedQueriesBeforeNextWrite(expected_queries_before_write);
}

DuckdbRuntime::~DuckdbRuntime() { UnregisterActiveRuntime(this); }

DuckdbRuntime *DuckdbRuntime::FindActive(::duckdb::DatabaseInstance &db,
                                         const std::string &accelerator_id) {
  std::shared_lock<std::shared_mutex> lock(ActiveRuntimeMutex());
  for (const auto &active : ActiveRuntimes()) {
    if (active.db == &db && active.accelerator_id == accelerator_id) {
      return active.runtime;
    }
  }
  return nullptr;
}

void DuckdbRuntime::Build(bool register_active) {
  accelerator_.runtime->Invalidate();
  if (!accelerator_.runtime->BuildNow()) {
    throw std::runtime_error(std::string("aux build did not sync: ") + entry_.accelerator_id);
  }
  if (register_active) {
    RegisterActiveRuntime(this, *connection_.context->db, entry_.accelerator_id);
    // Readable now: fill the table function's schema cache so the first read
    // doesn't plan the base SQL.
    PrimeAuxSchema(connection_, entry_);
  }
}

void DuckdbRuntime::EnsureBuilt() {
  if (!accelerator_.runtime->IsSynced()) {
    accelerator_.runtime->BuildNow();
  }
}

// ---- reads ----

template <typename Sink>
void DuckdbRuntime::Read(const core::QueryArguments &arguments, Sink &sink) {
  auto *reader = DuckDBAuxBackend::CurrentReader();
  // The table function's bind swaps in the SQL for such a transaction; this
  // is a statement bound before its first write. No state holds those rows,
  // and a side connection cannot see them: refuse rather than answer wrongly.
  if (reader && host_->TransactionWroteDependency(*reader, entry_.accelerator_id)) {
    throw std::runtime_error(std::string("accelerator ") + entry_.accelerator_id +
                             " cannot answer inside a transaction that wrote its tables; "
                             "run the query SQL directly");
  }
  runtime::ReadOptions options;
  const auto snapshot = reader ? DuckDBAuxBackend::SnapshotEpoch(*reader) : 0;
  if (snapshot != 0) { // 0: no transaction, the newest state
    options.epoch = snapshot;
  }
  options.max_wait_us = ReadWaitUs();
  // Scans never run on the accelerator's connection: a build may hold its
  // (older) snapshot open there. They read the reader's snapshot, else the
  // newest data.
  std::optional<ScanRedirect> redirect;
  if (reader != accelerator_.connection->context.get()) {
    redirect.emplace(*connection_.context->db, reader_fallbacks,
                     static_cast<::duckdb::transaction_t>(snapshot));
  }
  auto guard = accelerator_.runtime->BeginRead(options);
  t_table_scans = 0;
  if (guard.Servable()) {
    accelerator_.binding->Execute(*accelerator_.backend, arguments, sink);
  } else {
    accelerator_.binding->Module().ExecuteFallback(*accelerator_.backend, arguments, sink);
  }
  core::RecordAuxExecute(!guard.Servable() || t_table_scans > 0
                             ? core::AuxExecutePath::Fallback
                             : core::AuxExecutePath::Hit);
}

void DuckdbRuntime::ExecuteInto(const core::QueryArguments &arguments, core::AuxRowSink &sink) {
  Read(arguments, sink);
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect() {
  return ExecuteCollect(ToCoreArguments(entry_.benchmark_arguments));
}

std::vector<core::AuxRow> DuckdbRuntime::ExecuteCollect(const core::QueryArguments &arguments) {
  MaterializeSink sink;
  Read(arguments, sink);
  last_rows = sink.rows.size();
  return std::move(sink.rows);
}

void DuckdbRuntime::Execute() { Execute(ToCoreArguments(entry_.benchmark_arguments)); }

void DuckdbRuntime::Execute(const core::QueryArguments &arguments) {
  core::BeginAuxExecuteProfiling();
  ExecuteCollect(arguments);
  core::EndAuxExecuteProfiling();
}

// ---- writes (captured by the host whatever connection writes) ----

void DuckdbRuntime::BeginWrite() { connection_.BeginTransaction(); }

void DuckdbRuntime::CommitWrite() {
  try {
    connection_.Commit();
  } catch (...) {
    if (connection_.HasActiveTransaction()) {
      connection_.Rollback();
    }
    throw;
  }
}

void DuckdbRuntime::RollbackWrite() { connection_.Rollback(); }

uint64_t DuckdbRuntime::ApplyWrite(const std::string &sql) {
  BeginWrite();
  // Statement by statement, so each INSERT / UPDATE / DELETE reports its count.
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

void DuckdbRuntime::ApplyWriteAll(const std::vector<DuckdbRuntime *> &runtimes,
                                  const std::string &sql, WriteAllTiming *timing) {
  if (runtimes.empty()) {
    throw std::invalid_argument("ApplyWriteAll: no runtimes");
  }
  std::vector<runtime::AcceleratorStats> before;
  for (auto *rt : runtimes) {
    before.push_back(rt->Stats());
  }
  auto &connection = runtimes.front()->connection_;
  auto start = Clock::now();
  connection.BeginTransaction();
  RunWriteQuery(connection, sql);
  const double statement_us = MicrosSince(start);
  start = Clock::now();
  runtimes.front()->CommitWrite();
  if (!timing) {
    return;
  }
  *timing = WriteAllTiming{};
  timing->statement_us = statement_us;
  timing->engine_commit_us = MicrosSince(start);
  for (std::size_t i = 0; i < runtimes.size(); i++) {
    const auto after = runtimes[i]->Stats();
    const bool applied = after.commits_applied > before[i].commits_applied;
    timing->pre_commit_us.push_back(after.apply_us - before[i].apply_us);
    timing->commit_us.push_back(0);
    const auto &decision = applied ? after.last_decision : std::nullopt;
    timing->action.push_back(decision ? maintenance::MaintenanceActionName(decision->staged_action)
                                      : "none");
    timing->decision.push_back(decision ? *decision : core::MaintenanceDecisionLogEntry{});
  }
}

void DuckdbRuntime::Apply(SharedProbe &probe) {
  TimedMaintenanceWrite([&] {
    BeginWrite();
    RunWriteQuery(connection_, ProbeWriteSql(probe));
    CaptureByHand(BuildSharedMutation(entry_, probe));
    CommitWrite();
    AdvanceProbe(probe);
  });
}

void DuckdbRuntime::ApplyBatch(std::vector<SharedProbe> &probes) {
  if (probes.empty()) {
    throw std::invalid_argument("ApplyBatch requires at least one probe");
  }
  TimedMaintenanceWrite([&] {
    BeginWrite();
    for (auto &probe : probes) {
      RunWriteQuery(connection_, ProbeWriteSql(probe));
      CaptureByHand(BuildSharedMutation(entry_, probe));
      AdvanceProbe(probe);
    }
    CommitWrite();
  });
}

void DuckdbRuntime::ApplyBatchWrite(const std::string &sql, const core::MutationBatch &mutation) {
  TimedMaintenanceWrite([&] {
    BeginWrite();
    RunWriteQuery(connection_, sql);
    CaptureByHand(mutation);
    CommitWrite();
  });
}

void DuckdbRuntime::SetExpectedQueriesBeforeNextWrite(double expected_queries_before_write) {
  accelerator_.expected_queries_before_write = expected_queries_before_write;
}

double DuckdbRuntime::ExpectedQueriesBeforeNextWrite() const {
  return accelerator_.expected_queries_before_write;
}

uint64_t DuckdbRuntime::DataScaleRows() const { return accelerator_.runtime->DataScaleRows(); }

std::vector<core::CalibrationWrite> DuckdbRuntime::CalibrationWrites() const {
  return accelerator_.binding->Module().CalibrationWrites();
}

// ---- calibration and tests ----

void DuckdbRuntime::ResetCalibrationState() { accelerator_.runtime->Invalidate(); }

void DuckdbRuntime::InvalidateAll() { accelerator_.runtime->Invalidate(); }

runtime::ColumnarBatchRef DuckdbRuntime::ToBatch(const core::MutationBatch &mutation) const {
  std::vector<int32_t> attnums;
  for (const auto &dependency : accelerator_.binding->Module().Dependencies()) {
    if (dependency.table == mutation.table) {
      attnums = dependency.mutation_attnums;
    }
  }
  if (attnums.empty()) { // full-row dependency: the rows hold every column, in order
    const auto &rows = mutation.new_rows.empty() ? mutation.old_rows : mutation.new_rows;
    attnums.resize(rows.empty() ? 0 : rows.front().values.size());
    std::iota(attnums.begin(), attnums.end(), 0);
  }
  auto batch = runtime::FromMutationBatch(mutation, attnums);
  batch.table = accelerator_.tables.at(mutation.table);
  return std::make_shared<const runtime::ColumnarBatch>(std::move(batch));
}

void DuckdbRuntime::CaptureByHand(const core::MutationBatch &mutation) {
  if (!host_->Captures(accelerator_.tables.at(mutation.table))) {
    host_->CaptureByHand(*connection_.context, ToBatch(mutation));
  }
}

void DuckdbRuntime::ApplyMutationBatch(const core::MutationBatch &mutation) {
  // Not an engine transaction: published at the epoch of a transaction begun
  // now (after every commit so far). If the state goes stale, its rebuild
  // completes before returning, from a snapshot that includes this change.
  auto &runtime = *accelerator_.runtime;
  runtime.BeginCommit(0, {ToBatch(mutation)});
  runtime.PublishCommit(CurrentEpoch(connection_));
  if (!runtime.IsSynced()) {
    runtime.BuildNow();
  }
}

void DuckdbRuntime::UseManualCapture() { host_->SetManualCapture(true); }

void DuckdbRuntime::ExecuteBaseQuery() {
  // A Verus kit's SQL is parameterized ($1, ...): its own fallback runs it
  // with the benchmark arguments.
  if (IsVerusKitAccelerator(entry_.accelerator_id)) {
    MaterializeSink sink;
    accelerator_.binding->Module().ExecuteFallback(*accelerator_.backend, ToCoreArguments(entry_.benchmark_arguments), sink);
    return;
  }
  auto result = connection_.Query(entry_.base_sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(result ? result->GetError() : "DuckDB base query failed");
  }
  for (::duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (::duckdb::idx_t column = 0; column < result->ColumnCount(); ++column) {
      (void)result->GetValue(column, row);
    }
  }
}

// ---- diagnostics ----

namespace {

const VersionedAuxStore *AsVersioned(runtime::AcceleratorRuntime &runtime) {
  return dynamic_cast<const VersionedAuxStore *>(&runtime.StateStore());
}

const VerusAuxStore *AsVerus(runtime::AcceleratorRuntime &runtime) {
  return dynamic_cast<const VerusAuxStore *>(&runtime.StateStore());
}

} // namespace

bool DuckdbRuntime::UsesVerusStore() const { return AsVerus(*accelerator_.runtime) != nullptr; }

std::size_t DuckdbRuntime::StateVersionCount() const {
  if (const auto *verus = AsVerus(*accelerator_.runtime)) {
    return verus->TotalVersionCount();
  }
  return AsVersioned(*accelerator_.runtime)->TotalVersionCount();
}

std::size_t DuckdbRuntime::StaleInstallCount() const {
  const auto *verus = AsVerus(*accelerator_.runtime);
  return verus ? verus->StaleInstalls() : 0;
}

core::AuxStoreDiagnosticsSnapshot DuckdbRuntime::StoreDiagnosticsSnapshot() const {
  std::size_t valid, total;
  if (const auto *verus = AsVerus(*accelerator_.runtime)) {
    valid = verus->ValidPartitionCount();
    total = verus->TotalPartitionCount();
  } else {
    valid = AsVersioned(*accelerator_.runtime)->ValidPartitionCount();
    total = AsVersioned(*accelerator_.runtime)->TotalPartitionCount();
  }
  return core::AuxStoreDiagnosticsCollector::ThreadLocal().Snapshot(valid, total);
}

void DuckdbRuntime::ResetStoreDiagnosticsProfiling() { core::ResetAuxStoreDiagnosticsProfiling(); }

uint64_t DuckdbRuntime::AuxStateFootprintBytes() const {
  const auto *versioned = AsVersioned(*accelerator_.runtime);
  return versioned ? ComputeAuxStateFootprintBytes(*versioned, entry_.accelerator_id) : 0;
}

runtime::AcceleratorStats DuckdbRuntime::Stats() const { return accelerator_.runtime->Stats(); }

core::CalibrationEngine::Timed
DuckdbCalibrationEngine::WriteThenRead(const std::string &sql,
                                       std::optional<maintenance::MaintenanceAction> forced) {
  core::SetForcedMaintenanceActionName(forced ? maintenance::MaintenanceActionName(*forced) : "");
  core::ClearMaintenanceDecisionLog();
  Timed timed;
  const auto start = Clock::now();
  try {
    runtime.ApplyWrite(sql);
    const auto read_start = Clock::now();
    runtime.Execute();
    timed.read_us = MicrosSince(read_start);
  } catch (...) {
    core::SetForcedMaintenanceActionName("");
    throw;
  }
  timed.us = MicrosSince(start);
  core::SetForcedMaintenanceActionName("");
  timed.decision = core::LastMaintenanceDecision();
  return timed;
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
