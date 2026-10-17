// GENERATED BACKEND
#include "backends/duckdb/api/duckdb_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "backends/duckdb/api/aux_query_registry.hpp"
#include "backends/duckdb/api/cost_model_path.hpp"
#include "backends/duckdb/api/sql_fallback.hpp"
#include "backends/duckdb/api/verus_host.hpp"
#include "backends/duckdb/engine/capture.hpp"
#include "backends/duckdb/engine/scan.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "duckdb/accelerator/capture_registry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {
namespace {

using runtime::Epoch;

uint64_t TxnId(::duckdb::MetaTransaction &transaction) {
  return reinterpret_cast<uintptr_t>(&transaction);
}

double DefaultExpectedQueries() {
  const char *value = std::getenv("ACCELERATOR_CODEGEN_DUCKDB_EXPECTED_QUERIES_BEFORE_WRITE");
  return value && *value ? std::max(0.0, std::stod(value)) : 10.0;
}

std::unique_ptr<DuckdbRuntime::Binding> CreateBinding(::duckdb::Connection &connection,
                                                      const SharedQueryCatalogEntry &entry) {
  if (auto kit = MakeVerusKitBinding(connection, entry.accelerator_id)) {
    return kit;
  }
  const auto *query = FindAuxQuery(entry.accelerator_id);
  if (!query || !query->make_binding) {
    throw std::invalid_argument(std::string("no aux query registered for accelerator ") +
                                entry.accelerator_id);
  }
  return query->make_binding(connection);
}

// The columns to capture for `dependency` on `table`: its projection, or
// every column for a full-row dependency.
std::vector<int32_t> WatchedAttnums(::duckdb::Connection &connection, const std::string &table,
                                    const core::TableDependency &dependency) {
  if (!dependency.mutation_attnums.empty()) {
    return dependency.mutation_attnums;
  }
  std::vector<int32_t> all;
  if (auto info = connection.TableInfo(table)) {
    for (std::size_t i = 0; i < info->columns.size(); i++) {
      all.push_back(static_cast<int32_t>(i));
    }
  }
  return all;
}

// The build's read: a transaction on the accelerator's connection. Beginning
// and ending it take the scan lock: a fallback read streaming on this
// connection would be closed under its reader.
struct DuckdbBuildSnapshot final : runtime::BuildSnapshot {
  explicit DuckdbBuildSnapshot(::duckdb::Connection &connection_p) : connection(connection_p) {
    const auto lock = scan::LockScan(connection);
    connection.BeginTransaction();
    epoch = DuckDBAuxBackend::SnapshotEpoch(*connection.context);
  }
  ~DuckdbBuildSnapshot() override {
    const auto lock = scan::LockScan(connection);
    if (connection.HasActiveTransaction()) {
      connection.Rollback();
    }
  }
  Epoch SnapshotEpoch() const override { return epoch; }
  ::duckdb::Connection &connection;
  Epoch epoch = 0;
};

} // namespace

struct DuckdbHost::Impl final : runtime::EngineSlots {
  explicit Impl(::duckdb::DatabaseInstance &db_p)
      : db(db_p), capture(::duckdb::make_shared_ptr<DatabaseCapture>()),
        commit_ids(::duckdb::make_shared_ptr<CommitIds>()),
        runtime(*this, runtime::ResolveThreadBudget(
                           ::duckdb::DBConfig::GetConfig(db_p).options.maximum_threads)) {
    auto &threads = runtime.Threads();
    threads.SetPhaseLimits(runtime::ResolveReadThreads(0, threads.Budget()),
                           runtime::ResolveBuildThreads(0, threads.Budget()),
                           runtime::ResolveMaintenanceThreads(0, threads.Budget()));
  }

  ::duckdb::DatabaseInstance &db;
  ::duckdb::shared_ptr<DatabaseCapture> capture;
  ::duckdb::shared_ptr<CommitIds> commit_ids;
  ::duckdb::shared_ptr<::duckdb::AttachedDatabase> default_db; // set by the first Ensure
  // default_db for the engine's commit callbacks, which may run inside Ensure
  // (its own queries) and so must not take `mutex`.
  std::atomic<::duckdb::AttachedDatabase *> default_db_published{nullptr};

  mutable std::mutex mutex; // accelerators, tables, registered_at
  std::map<std::string, std::unique_ptr<Accelerator>> accelerators;
  // Host table ids: one per captured DuckDB table; tables an accelerator
  // captures by hand get ids of their own.
  std::unordered_map<std::string, core::TableId> tables; // by DuckDB table name
  core::TableId next_table = 1;
  std::atomic<bool> manual_capture{false};

  mutable std::mutex writers_mutex;
  std::unordered_map<uint64_t, Epoch> writers; // txn -> start time
  // Start time when the newest accelerator registered: writers that began
  // earlier may have written its tables uncaptured, so its first build waits
  // them out.
  std::atomic<Epoch> registered_at{0};
  // Writing transactions that began before the latest registration, by txn: when
  // one commits after an accelerator's build snapshot it may hold uncaptured
  // writes the barrier did not see (a statement already running).
  std::unordered_map<uint64_t, Epoch> straddlers;
  std::unordered_map<::duckdb::ClientContext *, ::duckdb::transaction_t> commit_id_of;

  // Used by builds (TableRows), so declared before `runtime`: it outlives them.
  mutable std::mutex statistics_mutex;
  mutable std::unique_ptr<::duckdb::Connection> statistics_connection;

  // Declared last: torn down first, so no build outlives the accelerators.
  runtime::RuntimeHost runtime;

  Accelerator &For(const core::AcceleratorId &id) const {
    std::lock_guard<std::mutex> lock(mutex);
    return *accelerators.at(id);
  }

  ::duckdb::DuckTransactionManager *Manager() const {
    if (!default_db) {
      return nullptr;
    }
    auto &manager = default_db->GetTransactionManager();
    return manager.IsDuckTransactionManager() ? &manager.Cast<::duckdb::DuckTransactionManager>()
                                              : nullptr;
  }

  // ---- slots ----
  Epoch PendingEpochLowerBound(uint64_t txn) const override {
    std::lock_guard<std::mutex> lock(writers_mutex);
    auto it = writers.find(txn);
    return it == writers.end() ? 0 : it->second + 1;
  }
  Epoch OldestReaderEpoch() const override {
    auto *manager = Manager();
    return manager ? manager->LowestActiveStart() : kNoFloor;
  }
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &,
                                     uint64_t timeout_us) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    auto *manager = Manager();
    for (;;) {
      const auto starts = manager ? manager->ActiveWriterStarts() : std::vector<::duckdb::transaction_t>{};
      if (std::none_of(starts.begin(), starts.end(),
                       [&](::duckdb::transaction_t start) { return start < registered_at; })) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  std::unique_ptr<runtime::BuildSnapshot> OpenBuildSnapshot(runtime::AcceleratorRuntime &accelerator) override {
    return std::make_unique<DuckdbBuildSnapshot>(*For(accelerator.Id()).connection);
  }
  bool EngineWorkOnHostThread() const override { return false; }
  void RunBuild(runtime::AcceleratorRuntime &accelerator, runtime::BuildSnapshot &,
                core::StateBuilder &builder) override {
    auto &entry = For(accelerator.Id());
    entry.binding->Build(*entry.backend, builder);
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &id) const override {
    return For(id).cost_model;
  }
  double ExpectedQueriesBeforeNextWrite(const core::AcceleratorId &id) const override {
    return For(id).expected_queries_before_write;
  }
  uint64_t TableRows(core::TableId table) const override {
    std::string name;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto &entry : tables) {
        if (entry.second == table) {
          name = entry.first;
        }
      }
    }
    return name.empty() ? 0 : StoredRows(name);
  }

  // The table's stored rows (counting uncommitted deletes), from its storage.
  uint64_t StoredRows(const std::string &name) const {
    std::lock_guard<std::mutex> lock(statistics_mutex);
    if (!statistics_connection) {
      statistics_connection = std::make_unique<::duckdb::Connection>(db);
    }
    uint64_t rows = 0;
    statistics_connection->context->RunFunctionInTransaction([&] {
      auto &entry = ::duckdb::Catalog::GetEntry<::duckdb::TableCatalogEntry>(
          *statistics_connection->context, INVALID_CATALOG, DEFAULT_SCHEMA, name);
      rows = entry.GetStorage().GetTotalRows();
    });
    return rows;
  }
};

namespace {

std::mutex &HostsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<::duckdb::DatabaseInstance *, std::weak_ptr<DuckdbHost>> &Hosts() {
  static std::unordered_map<::duckdb::DatabaseInstance *, std::weak_ptr<DuckdbHost>> hosts;
  return hosts;
}

} // namespace

std::shared_ptr<DuckdbHost> DuckdbHost::Get(::duckdb::DatabaseInstance &db) {
  std::lock_guard<std::mutex> lock(HostsMutex());
  auto &slot = Hosts()[&db];
  if (auto host = slot.lock()) {
    return host;
  }
  std::shared_ptr<DuckdbHost> host(new DuckdbHost(db));
  host->Attach(host);
  slot = host;
  return host;
}

std::shared_ptr<DuckdbHost> DuckdbHost::Existing(::duckdb::DatabaseInstance &db) {
  std::lock_guard<std::mutex> lock(HostsMutex());
  auto it = Hosts().find(&db);
  return it == Hosts().end() ? nullptr : it->second.lock();
}

DuckdbHost::DuckdbHost(::duckdb::DatabaseInstance &db) : impl(std::make_unique<Impl>(db)) {}

// Starts the engine's callbacks into the host (which must already be shared).
void DuckdbHost::Attach(const std::shared_ptr<DuckdbHost> &self) {
  impl->capture->Bind(self);
  impl->commit_ids->Bind(self);
  auto &registry = impl->db.GetCaptureRegistry();
  registry.Add(impl->capture);
  registry.Add(impl->commit_ids);
}

DuckdbHost::~DuckdbHost() {
  auto &registry = impl->db.GetCaptureRegistry();
  registry.Remove(impl->capture.get());
  registry.Remove(impl->commit_ids.get());
  auto &watched = WatchedColumns(impl->db);
  for (auto &[id, accelerator] : impl->accelerators) {
    for (const auto &[name, table] : accelerator->binding->CaptureTables()) {
      for (const auto &dependency : accelerator->binding->Module().Dependencies()) {
        if (dependency.table == table) {
          watched.Remove(name, WatchedAttnums(*accelerator->connection, name, dependency));
        }
      }
    }
  }
}

DuckdbHost::Accelerator &DuckdbHost::Ensure(const SharedQueryCatalogEntry &entry) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto &slot = impl->accelerators[entry.accelerator_id];
  if (slot) {
    return *slot;
  }
  auto accelerator = std::make_unique<Accelerator>();
  accelerator->entry = &entry;
  accelerator->connection = std::make_unique<::duckdb::Connection>(impl->db);
  if (!impl->default_db) {
    auto &context = *accelerator->connection->context;
    impl->default_db = ::duckdb::DatabaseManager::Get(context).GetDatabase(
        ::duckdb::DatabaseManager::GetDefaultDatabase(context));
    impl->default_db_published = impl->default_db.get();
  }
  accelerator->binding = CreateBinding(*accelerator->connection, entry);
  accelerator->cost_model = core::LoadAuxCostModelJson(SharedCostModelConfigPath(entry));
  accelerator->expected_queries_before_write = DefaultExpectedQueries();
  auto &module = accelerator->binding->Module();
  runtime::StoreFactory verus_if_requested = [](const core::AcceleratorId &id,
                                                std::function<Epoch()> epoch,
                                                std::function<Epoch()> floor) {
    runtime::StoreHandle handle;
    if (auto verus = MakeRequestedVerusStore(id, epoch, floor)) {
      auto *store = verus.get();
      handle.install = [store, id](std::vector<core::StateImage> images, Epoch at) {
        store->Install(std::move(images), id, at);
      };
      handle.store = std::move(verus);
    }
    return handle;
  };
  const auto capture_tables = accelerator->binding->CaptureTables();
  for (const auto &[name, table] : capture_tables) {
    auto [it, added] = impl->tables.emplace(name, impl->next_table);
    impl->next_table += added;
    accelerator->tables[table] = it->second;
  }
  for (const auto &dependency : module.Dependencies()) {
    if (accelerator->tables.emplace(dependency.table, impl->next_table).second) {
      impl->next_table++;
    }
  }
  accelerator->runtime = &impl->runtime.Register(
      std::shared_ptr<core::AuxQueryModule>(&module, [](core::AuxQueryModule *) {}),
      verus_if_requested, accelerator->tables);
  accelerator->backend =
      std::make_unique<DuckDBAuxBackend>(*accelerator->runtime, *accelerator->connection->context);
  accelerator->backend->SetSqlAnswer(
      [connection = accelerator->connection.get(), &entry](const core::QueryArguments &arguments,
                                                           core::AuxRowSink &sink) {
        return AnswerFromFallbackSql(*connection, entry, arguments, sink);
      });
  accelerator->runtime->UseModuleServices(*accelerator->backend);

  // Watch its columns from now on; writers that started before may have
  // written them uncaptured (S5's registration barrier waits them out).
  auto &watched = WatchedColumns(impl->db);
  for (const auto &[name, table] : capture_tables) {
    for (const auto &dependency : module.Dependencies()) {
      if (dependency.table == table) {
        watched.Add(name, WatchedAttnums(*accelerator->connection, name, dependency));
      }
    }
  }
  accelerator->connection->BeginTransaction();
  impl->registered_at = DuckDBAuxBackend::SnapshotEpoch(*accelerator->connection->context);
  accelerator->connection->Rollback();
  accelerator->registered_at = impl->registered_at.load();

  slot = std::move(accelerator);
  return *slot;
}

DuckdbHost::Accelerator *DuckdbHost::Find(const std::string &accelerator_id) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  auto it = impl->accelerators.find(accelerator_id);
  return it == impl->accelerators.end() ? nullptr : it->second.get();
}

runtime::RuntimeHost &DuckdbHost::Runtime() { return impl->runtime; }

bool DuckdbHost::TransactionWroteDependency(::duckdb::ClientContext &context,
                                            const std::string &accelerator_id) {
  auto *accelerator = Find(accelerator_id);
  return accelerator && impl->runtime.TxnWroteDependency(
                            TxnId(::duckdb::MetaTransaction::Get(context)), *accelerator->runtime);
}

void DuckdbHost::SetManualCapture(bool manual) { impl->manual_capture = manual; }

bool DuckdbHost::Captures(core::TableId table) {
  std::lock_guard<std::mutex> lock(impl->mutex);
  return std::any_of(impl->tables.begin(), impl->tables.end(),
                     [&](const auto &entry) { return entry.second == table; });
}

void DuckdbHost::CaptureByHand(::duckdb::ClientContext &context, runtime::ColumnarBatchRef batch) {
  const auto txn = TxnId(::duckdb::MetaTransaction::Get(context));
  const Epoch start = DuckDBAuxBackend::SnapshotEpoch(context); // engine call: before writers_mutex
  {
    std::lock_guard<std::mutex> lock(impl->writers_mutex);
    impl->writers.emplace(txn, start);
  }
  impl->runtime.OnCapture(txn, 0, std::move(batch));
}

// ---- engine callbacks ----

void DuckdbHost::OnMutation(::duckdb::ClientContext &context,
                            const ::duckdb::TableMutationEvent &event) {
  if (impl->manual_capture) {
    return;
  }
  core::TableId table;
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto it = impl->tables.find(event.table_name);
    if (it == impl->tables.end() || event.row_count == 0) {
      return;
    }
    table = it->second;
  }
  const auto txn = TxnId(::duckdb::MetaTransaction::Get(context));
  const Epoch start = DuckDBAuxBackend::SnapshotEpoch(context); // engine call: before writers_mutex
  {
    std::lock_guard<std::mutex> lock(impl->writers_mutex);
    impl->writers.emplace(txn, start);
  }
  impl->runtime.OnCapture(txn, 0,
                          std::make_shared<const runtime::ColumnarBatch>(ToColumnarBatch(event, table)));
}

void DuckdbHost::OnPreCommit(::duckdb::MetaTransaction &transaction) {
  const auto txn = TxnId(transaction);
  // Read from the engine before taking writers_mutex: DuckDB's commit path calls
  // OnCommitId with its own locks held, so ours never waits on one of its locks.
  auto *default_db = impl->default_db_published.load();
  std::optional<::duckdb::transaction_t> write_start;
  if (default_db) {
    auto db_transaction = transaction.TryGetTransaction(*default_db);
    if (db_transaction && db_transaction->IsDuckTransaction() && !db_transaction->IsReadOnly()) {
      write_start = db_transaction->Cast<::duckdb::DuckTransaction>().start_time;
    }
  }
  bool captured;
  {
    std::lock_guard<std::mutex> lock(impl->writers_mutex);
    if (write_start && *write_start < impl->registered_at) {
      impl->straddlers[txn] = *write_start;
    }
    captured = impl->writers.count(txn) != 0;
  }
  if (captured) {
    impl->runtime.PreCommit(txn);
  }
}

void DuckdbHost::OnCommitId(::duckdb::ClientContext &context, ::duckdb::AttachedDatabase &db,
                            ::duckdb::transaction_t commit_id) {
  if (db.IsSystem() || db.IsTemporary()) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl->writers_mutex);
  impl->commit_id_of[&context] = commit_id;
}

void DuckdbHost::OnCommit(::duckdb::MetaTransaction &transaction, ::duckdb::ClientContext &context) {
  const auto txn = TxnId(transaction);
  ::duckdb::transaction_t commit_id = 0;
  bool captured, has_id;
  std::optional<Epoch> straddled_from;
  {
    std::lock_guard<std::mutex> lock(impl->writers_mutex);
    auto id = impl->commit_id_of.find(&context);
    has_id = id != impl->commit_id_of.end();
    if (has_id) {
      commit_id = id->second;
      impl->commit_id_of.erase(id);
    }
    captured = impl->writers.erase(txn) != 0;
    if (auto it = impl->straddlers.find(txn); it != impl->straddlers.end()) {
      straddled_from = it->second;
      impl->straddlers.erase(it);
    }
    if (captured && !has_id) {
      throw std::logic_error("a captured write committed without a commit id");
    }
  }
  // Visible to snapshots with start time > commit id.
  if (captured) {
    impl->runtime.Commit(txn, commit_id + 1);
  }
  if (straddled_from && has_id) {
    InvalidateStraddled(*straddled_from, commit_id + 1);
  }
}

// A writer that began before an accelerator registered commits at `epoch`: if the
// accelerator's build snapshot predates it, what the writer did before the
// registration is in neither the build nor a capture.
void DuckdbHost::InvalidateStraddled(Epoch start, Epoch epoch) {
  std::vector<runtime::AcceleratorRuntime *> registered_after;
  {
    std::lock_guard<std::mutex> lock(impl->mutex); // not held across InvalidateFrom: builds take it too
    for (auto &[id, accelerator] : impl->accelerators) {
      if (start < accelerator->registered_at) {
        registered_after.push_back(accelerator->runtime);
      }
    }
  }
  for (auto *accelerator : registered_after) {
    accelerator->InvalidateFrom(epoch);
  }
}

void DuckdbHost::OnRollback(::duckdb::MetaTransaction &transaction) {
  const auto txn = TxnId(transaction);
  {
    std::lock_guard<std::mutex> lock(impl->writers_mutex);
    impl->straddlers.erase(txn);
    if (!impl->writers.erase(txn)) {
      return;
    }
  }
  impl->runtime.Abort(txn);
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
