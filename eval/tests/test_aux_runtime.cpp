// The runtime skeleton (substrate/aux/runtime) against a fake engine.
//
// The fake database is a list of committed deltas; the accelerator keeps their
// sum. Every read is checked against the sum of the commits its epoch sees.
//
//  1. In-process commits: staged at pre-commit, published at the commit
//     epoch, old epochs keep reading their versions, aborts and
//     subtransaction aborts change nothing.
//  2. Commits during a background build are caught up; readers in the
//     collapsed range fall back, never read stale.
//  3. A module that asks for a rebuild gets one in the background.
//  4. Shipped commits (Postgres placement): out of order, gaps, a reader that
//     needs an unapplied commit waits or falls back, a gap rebuilds.
//  5. Thread limits per phase: reads and builds each get their own cap,
//     maintenance runs ParallelFor serially.
//  6. Maintenance lookups that see more or fewer commits than were shipped
//     (EngineSlots::MaintenanceLookupEpoch): applied as one combined change,
//     or waited for.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::runtime;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " +     \
                               #cond);                                                             \
    }                                                                                              \
  } while (0)

namespace {

constexpr core::TableId kTable = 7;

// ---- fake database ----

struct FakeDb {
  std::mutex mutex;
  explicit FakeDb(Epoch start = 100) : clock(start) {}

  std::vector<std::pair<Epoch, int64_t>> commits; // (epoch, delta)
  Epoch clock;

  Epoch Commit(int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex);
    clock += 7; // sparse epochs, like DuckDB commit ids
    commits.emplace_back(clock, delta);
    return clock;
  }
  Epoch Now() {
    std::lock_guard<std::mutex> lock(mutex);
    return clock;
  }
  int64_t SumAt(Epoch epoch) {
    std::lock_guard<std::mutex> lock(mutex);
    int64_t sum = 0;
    for (const auto &[e, delta] : commits) {
      if (e <= epoch) {
        sum += delta;
      }
    }
    return sum;
  }
};

// ---- fake module: SELECT sum(x) ----

struct SumBuffer final : core::AuxTxnBuffer {
  int64_t delta = 0;
  bool pending = false;
  bool HasPendingChanges() const override { return pending; }
  void Clear() override { *this = SumBuffer(); }
};

struct SumModule final : core::AuxQueryModule {
  bool request_rebuild = false; // StageMaintenance asks for RebuildAll
  std::function<void(core::AuxEngineServices &)> on_execute, on_stage; // test probes

  core::AcceleratorId Id() const override { return "sum"; }
  std::vector<core::TableDependency> Dependencies() const override {
    return {{kTable, {0}, {0}}};
  }
  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<SumBuffer>();
  }
  void Build(core::AuxEngineServices &, core::StateBuilder &) override {
    throw std::logic_error("built through EngineSlots::RunBuild");
  }
  core::InvalidationPlan InterpretMutation(const core::MutationBatch &) const override {
    core::InvalidationPlan plan;
    plan.scope = InvalidationScope::PartitionSet;
    plan.partitions.insert(0);
    return plan;
  }
  void AccumulateMutation(const core::MutationBatch &batch, core::AuxEngineServices &,
                          core::AuxTxnBuffer &buffer) override {
    auto &sum = static_cast<SumBuffer &>(buffer);
    const int64_t sign = batch.kind == core::MutationKind::Delete ? -1 : 1;
    for (const auto &row : (sign < 0 ? batch.old_rows : batch.new_rows)) {
      sum.delta += sign * std::get<int64_t>(row.values[0].payload);
    }
    sum.pending = true;
  }
  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &services,
                                                  const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &buffer) override {
    core::StagedMaintenanceOutcome outcome;
    if (on_stage) {
      on_stage(services);
    }
    if (request_rebuild) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    const auto current = services.StateStore().Read(services.CurrentVisibility());
    CHECK(current.partitions.size() == 1);
    const int64_t next = *current.partitions[0].As<int64_t>() +
                         static_cast<SumBuffer &>(buffer).delta;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(
        core::StateImage::Make<int64_t>(Id(), 0, 0, std::make_shared<const int64_t>(next)));
    return outcome;
  }
  bool CheckValidity(core::AuxEngineServices &services, const core::QueryArguments &) const override {
    return services.StateStore()
        .CheckValidity(core::QueryKey{Id(), ""}, services.CurrentVisibility())
        .IsValid();
  }
  void Execute(core::AuxEngineServices &services, const core::QueryArguments &,
               core::AuxRowSink &sink) override {
    if (on_execute) {
      on_execute(services);
    }
    const auto state = services.StateStore().Read(services.CurrentVisibility());
    core::AuxRow row;
    row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, *state.partitions[0].As<int64_t>()));
    sink.WriteRow(row);
  }
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &,
                       core::AuxRowSink &) override {}
};

// ---- fake slots ----

struct FakeSnapshot final : BuildSnapshot {
  explicit FakeSnapshot(Epoch e) : epoch(e) {}
  Epoch SnapshotEpoch() const override { return epoch; }
  Epoch epoch;
};

struct FakeSlots final : EngineSlots {
  explicit FakeSlots(FakeDb &db_p, bool host_thread = false) : db(db_p), host_thread(host_thread) {}

  Epoch PendingEpochLowerBound(uint64_t) const override { return db.Now() + 1; }
  Epoch OldestReaderEpoch() const override { return floor; }
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) override {
    return true;
  }
  std::unique_ptr<BuildSnapshot> OpenBuildSnapshot(AcceleratorRuntime &) override {
    if (fail_builds) {
      throw std::runtime_error("no build snapshot");
    }
    return std::make_unique<FakeSnapshot>(build_at.value_or(db.Now()));
  }
  bool EngineWorkOnHostThread() const override { return host_thread; }
  void RunBuild(AcceleratorRuntime &runtime, BuildSnapshot &snapshot,
                core::StateBuilder &builder) override {
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      building = true;
      gate_cv.notify_all();
      gate_cv.wait(lock, [&] { return !hold_builds; });
      building = false;
    }
    if (on_build) {
      on_build(runtime);
    }
    const int64_t sum = db.SumAt(snapshot.SnapshotEpoch());
    builder.Write(core::StateImage::Make<int64_t>(runtime.Id(), 0, 0,
                                                  std::make_shared<const int64_t>(sum)));
    builder.Finish();
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override {
    return params;
  }
  Epoch MaintenanceLookupEpoch(Epoch through) override { return lookup.value_or(through); }
  uint64_t TableRows(core::TableId table) const override { return table == kTable ? table_rows : 0; }

  uint64_t table_rows = 0;

  // The epoch the engine's maintenance lookups see (default: the change's).
  std::optional<Epoch> lookup;
  Epoch floor = 1; // keeps every version
  std::optional<Epoch> build_at; // the next builds' snapshot (default: now)

  // Holds builds inside RunBuild until Release().
  void Hold() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    hold_builds = true;
  }
  void WaitUntilBuilding() {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_cv.wait(lock, [&] { return building; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(gate_mutex);
    hold_builds = false;
    gate_cv.notify_all();
  }

  FakeDb &db;
  bool host_thread;
  std::function<void(AcceleratorRuntime &)> on_build; // test probe
  maintenance::CostModelParams params;
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool hold_builds = false;
  bool building = false;
  std::atomic<bool> fail_builds{false};
};

struct CollectSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { values.push_back(std::get<int64_t>(row.values[0].payload)); }
  void End() override {}
  std::vector<int64_t> values;
};

ColumnarBatchRef Insert(int64_t value) {
  auto batch = std::make_shared<ColumnarBatch>();
  batch->table = kTable;
  batch->kind = core::MutationKind::Insert;
  batch->row_count = 1;
  batch->new_rows.attnums = {0};
  ColumnData column;
  column.type = core::AuxType::Int64;
  column.values = std::vector<int64_t>{value};
  batch->new_rows.columns.push_back(std::move(column));
  return batch;
}

// Reads at `epoch`; nullopt if the runtime fell back.
std::optional<int64_t> ReadAt(AcceleratorRuntime &runtime, Epoch epoch,
                              std::optional<Epoch> applied_through = std::nullopt,
                              uint64_t wait_us = 2'000'000) {
  ReadOptions options;
  options.epoch = epoch;
  options.require_applied_through = applied_through;
  options.max_wait_us = wait_us;
  CollectSink sink;
  if (runtime.Read(options, {}, sink) == ReadResult::Fallback) {
    return std::nullopt;
  }
  CHECK(sink.values.size() == 1);
  return sink.values[0];
}

// One in-process write transaction adding `delta`.
Epoch WriteTxn(RuntimeHost &host, FakeDb &db, uint64_t txn, int64_t delta) {
  host.OnCapture(txn, 0, Insert(delta));
  host.PreCommit(txn);
  const Epoch epoch = db.Commit(delta);
  host.Commit(txn, epoch);
  return epoch;
}

void InProcessCommits() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  host.BuildAll();
  CHECK(runtime.IsSynced());

  const Epoch e1 = WriteTxn(host, db, 1, 5);
  const Epoch e2 = WriteTxn(host, db, 2, 7);
  CHECK(ReadAt(runtime, e1) == 5);
  CHECK(ReadAt(runtime, e2) == 12);
  CHECK(ReadAt(runtime, VersionedAuxStore::kNewestSnapshot) == 12);

  // Abort: nothing changes, the sequence stays dense.
  host.OnCapture(3, 0, Insert(100));
  host.PreCommit(3);
  host.Abort(3);
  // Subtransaction abort drops its batches.
  host.OnCapture(4, 0, Insert(1));
  host.OnCapture(4, 1, Insert(1000));
  host.AbortSubtransaction(4, 1);
  host.PreCommit(4);
  const Epoch e4 = db.Commit(1);
  host.Commit(4, e4);
  CHECK(ReadAt(runtime, e4) == 13);
  CHECK(runtime.IsSynced());
  CHECK(runtime.Stats().commits_applied == 3);
}

// A snapshot that sees no commit (epoch 0, as on Postgres before the first
// one) is a snapshot, not "the newest": it reads the state built at 0, and
// falls back when the state was built later.
void ReaderBeforeFirstCommit() {
  {
    FakeDb db(0);
    FakeSlots slots(db);
    slots.floor = 0; // a reader is at epoch 0
    RuntimeHost host(slots, 4);
    auto &runtime = host.Register(std::make_shared<SumModule>());
    host.BuildAll();
    const Epoch e1 = WriteTxn(host, db, 1, 5);
    CHECK(ReadAt(runtime, 0) == 0);
    CHECK(ReadAt(runtime, e1) == 5);
  }
  FakeDb db(0);
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  const Epoch e1 = WriteTxn(host, db, 1, 5); // built at e1
  CHECK(ReadAt(runtime, e1) == 5);
  CHECK(!ReadAt(runtime, 0));
}

// Commits a failed apply left unpublished: after the rebuild, a reader in
// that window falls back instead of reading the older version.
void ReaderInStaleWindow() {
  FakeDb db(0);
  FakeSlots slots(db, /*host_thread=*/true);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  host.BuildAll();
  auto ship = [&](Seq seq, int64_t delta, bool invalidates) {
    db.commits.emplace_back(seq, delta);
    runtime.OfferCommit(CommittedChange{seq, seq, {Insert(delta)}, false, invalidates});
  };
  ship(1, 10, false);
  runtime.Pump();
  ship(2, 20, true); // cannot be maintained: stale from 2
  ship(3, 30, false);
  slots.build_at = 3;
  runtime.Pump();
  host.RunEngineWork();
  CHECK(runtime.IsSynced());
  CHECK(ReadAt(runtime, 1, Epoch{1}) == 10);
  CHECK(!ReadAt(runtime, 2, Epoch{2}, 1000));
  CHECK(ReadAt(runtime, 3, Epoch{3}) == 60);
}

// A commit no capture saw (a writer from before the accelerator registered)
// makes a state built before it stale from its epoch on, whether the state is
// installed or its build is still running; a state built after it is unaffected.
void UncapturedCommit() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  host.BuildAll();
  const Epoch e1 = WriteTxn(host, db, 1, 5);
  const Epoch lost = db.Commit(100); // not captured
  runtime.InvalidateFrom(lost);
  CHECK(ReadAt(runtime, e1) == 5); // a snapshot before it is unaffected
  CHECK(ReadAt(runtime, lost) == 105); // rebuilt
  CHECK(runtime.Stats().builds == 2);
  runtime.InvalidateFrom(lost); // the state's snapshot sees it
  CHECK(runtime.IsSynced() && runtime.Stats().builds == 2);

  FakeDb db2;
  FakeSlots slots2(db2);
  RuntimeHost host2(slots2, 4);
  auto &runtime2 = host2.Register(std::make_shared<SumModule>());
  slots2.Hold();
  bool synced = true;
  std::thread build([&] { synced = runtime2.BuildNow(); });
  slots2.WaitUntilBuilding(); // its snapshot is taken
  const Epoch lost2 = db2.Commit(7);
  runtime2.InvalidateFrom(lost2);
  slots2.Release();
  build.join();
  CHECK(!synced); // installed stale (the rebuild it requests may already be done)
  CHECK(ReadAt(runtime2, lost2) == 7);
  CHECK(runtime2.Stats().builds == 2);
}

// A host torn down while a commit is between PreCommit and Commit: the build
// queued behind that commit must not keep the pool from shutting down.
void HostDestroyedMidCommit() {
  FakeDb db;
  FakeSlots slots(db);
  auto host = std::make_unique<RuntimeHost>(slots, 4);
  auto &runtime = host->Register(std::make_shared<SumModule>());
  host->OnCapture(1, 0, Insert(5));
  host->PreCommit(1); // holds the accelerator's place in commit order
  runtime.RequestBuild(); // waits behind it
  std::promise<void> destroyed;
  std::thread destroyer([&] {
    host.reset();
    destroyed.set_value();
  });
  const bool done = destroyed.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  if (done) {
    destroyer.join();
  } else {
    destroyer.detach();
  }
  CHECK(done);
}

void CommitsDuringBuild() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  slots.Hold();
  WriteTxn(host, db, 1, 1); // stale: queued, and starts the first build
  slots.WaitUntilBuilding(); // its snapshot already includes commit 1
  const Epoch a = WriteTxn(host, db, 2, 10);
  const Epoch b = WriteTxn(host, db, 3, 100);
  CHECK(!ReadAt(runtime, b, std::nullopt, 0)); // not built yet: never stale
  slots.Release();
  CHECK(ReadAt(runtime, b) == 111);
  CHECK(!ReadAt(runtime, a, std::nullopt, 0)); // collapsed by the catch-up
  CHECK(runtime.Stats().catch_up_commits == 2);
  WriteTxn(host, db, 4, 1000);
  CHECK(ReadAt(runtime, VersionedAuxStore::kNewestSnapshot) == 1111);
}

void ModuleRequestsRebuild() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto module = std::make_shared<SumModule>();
  auto &runtime = host.Register(module);
  host.BuildAll();
  module->request_rebuild = true;
  const Epoch e = WriteTxn(host, db, 1, 42);
  module->request_rebuild = false;
  CHECK(ReadAt(runtime, e) == 42); // rebuilt in the background
  CHECK(runtime.Stats().builds == 2);
  CHECK(runtime.Stats().commits_stale == 1);
  CHECK(!runtime.Stats().last_stale_reason.empty());
}

// The policy sees the dependency tables' size: the ApplyDelta row cap and the
// rebuild costs scale with it, so a refresh that grows with the data is still
// applied.
void DataScaleScalesTheCap() {
  FakeDb db;
  FakeSlots slots(db);
  slots.params.calibrated_scale_rows = 1000;
  slots.params.apply_delta_max_rows = 1000;
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  core::MaintenanceRequest refresh;
  refresh.rows_touched = 5000;
  refresh.incremental_supported = true;
  CHECK(runtime.DataScaleRows() == 0);
  CHECK(runtime.ChooseMaintenance(refresh).action != maintenance::MaintenanceAction::ApplyDelta);
  slots.table_rows = 20'000; // 20x the calibrated size: cap 20'000 rows
  CHECK(runtime.DataScaleRows() == 20'000);
  CHECK(runtime.ChooseMaintenance(refresh).action == maintenance::MaintenanceAction::ApplyDelta);
}

void FailedBuildReleasesReaders() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  slots.fail_builds = true;
  const auto start = std::chrono::steady_clock::now();
  CHECK(!ReadAt(runtime, db.Now(), std::nullopt, 60'000'000)); // falls back when the build fails
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
  slots.fail_builds = false;
  CHECK(ReadAt(runtime, db.Now()) == 0); // the next read builds again
}

void InvalidateDuringBuild() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  slots.Hold();
  std::atomic<bool> failed{false};
  std::thread build([&] {
    try {
      runtime.BuildNow();
    } catch (const std::exception &) {
      failed = true;
    }
  });
  slots.WaitUntilBuilding();
  runtime.Invalidate(); // what the build scans may miss what invalidated
  slots.Release();
  build.join();
  CHECK(failed && !runtime.IsSynced()); // the build is not installed
  CHECK(ReadAt(runtime, db.Now()) == 0); // the next one is
}

void ShippedCommits() {
  FakeDb db(0); // shipped epochs are sequence numbers, from 0
  FakeSlots slots(db, /*host_thread=*/true);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  host.BuildAll();
  auto ship = [&](Seq seq, int64_t delta) {
    runtime.OfferCommit(CommittedChange{seq, seq, {Insert(delta)}, false});
  };
  ship(2, 20);
  ship(1, 10);
  runtime.Pump();
  CHECK(ReadAt(runtime, 2, Epoch{2}) == 30);
  runtime.AnnounceCommit(3); // committed, not yet shipped
  CHECK(!ReadAt(runtime, 3, Epoch{3}, 1000)); // must not read without it
  ship(4, 40);
  runtime.Pump();
  CHECK(!ReadAt(runtime, 4, Epoch{4}, 1000));
  ship(3, 30);
  runtime.Pump();
  CHECK(ReadAt(runtime, 4, Epoch{4}) == 100);
}

// Records the worker count a phase reports and how many threads its
// ParallelFor actually ran at once.
struct PhaseProbe {
  std::atomic<std::size_t> workers{0}, running{0}, peak{0};
  void Run(core::AuxEngineServices &services) {
    workers = services.WorkerCount();
    services.ParallelFor(32, [&](std::size_t) {
      const std::size_t now = ++running;
      std::size_t seen = peak;
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      --running;
    });
  }
};

void ThreadLimitsPerPhase() {
  FakeDb db;
  FakeSlots slots(db);
  RuntimeHost host(slots, 8);
  host.Threads().SetPhaseLimits(3, 2);
  auto module = std::make_shared<SumModule>();
  PhaseProbe read, build, stage;
  module->on_execute = [&](core::AuxEngineServices &s) { read.Run(s); };
  module->on_stage = [&](core::AuxEngineServices &s) { stage.Run(s); };
  slots.on_build = [&](AcceleratorRuntime &r) { build.Run(r); };
  auto &runtime = host.Register(module);
  host.BuildAll();
  const Epoch e = WriteTxn(host, db, 1, 5);
  CHECK(ReadAt(runtime, e) == 5);

  CHECK(build.workers == 2 && build.peak == 2);
  CHECK(read.workers == 3 && read.peak == 3);
  CHECK(stage.workers == 1 && stage.peak == 1);
  CHECK(runtime.WorkerCount() == 1); // outside any phase

  host.Threads().SetPhaseLimits(100, 0, 4); // capped by, and defaulting to, the budget
  const Epoch e2 = WriteTxn(host, db, 2, 7);
  CHECK(ReadAt(runtime, e2) == 12);
  CHECK(read.workers == 8);
  CHECK(stage.workers == 4 && stage.peak == 4);
}

void MaintenanceLookups() {
  FakeDb db(0);
  FakeSlots slots(db, /*host_thread=*/true);
  RuntimeHost host(slots, 4);
  auto &runtime = host.Register(std::make_shared<SumModule>());
  host.BuildAll();
  auto ship = [&](Seq seq, int64_t delta) {
    runtime.OfferCommit(CommittedChange{seq, seq, {Insert(delta)}, false});
  };

  // The lookups see exactly what arrived: all three apply as one change; the
  // epochs in between were never published, so readers there fall back.
  slots.lookup = 3;
  ship(1, 1);
  ship(2, 10);
  ship(3, 100);
  runtime.Pump();
  CHECK(ReadAt(runtime, 3, Epoch{3}) == 111);
  CHECK(!ReadAt(runtime, 2, std::nullopt, 0));
  CHECK(runtime.Stats().commits_applied == 3);

  // They see a commit that has not arrived yet: nothing applies until it does.
  slots.lookup = 5;
  ship(4, 1000);
  runtime.Pump();
  CHECK(!ReadAt(runtime, 4, Epoch{4}, 1000));
  CHECK(runtime.Stats().commits_applied == 3);
  ship(5, 10000);
  runtime.Pump();
  CHECK(ReadAt(runtime, 5, Epoch{5}) == 11111);

  // They see fewer than arrived: the visible ones apply, the rest waits for a
  // snapshot that sees it.
  slots.lookup = 6;
  ship(6, 100000);
  ship(7, 1000000);
  runtime.Pump();
  CHECK(ReadAt(runtime, 6, Epoch{6}) == 111111);
  CHECK(!ReadAt(runtime, 7, Epoch{7}, 1000));
  slots.lookup = 7;
  runtime.Pump();
  CHECK(ReadAt(runtime, 7, Epoch{7}) == 1111111);
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, void (*)()>> tests = {
      {"in_process_commits", InProcessCommits},
      {"commits_during_build", CommitsDuringBuild},
      {"module_requests_rebuild", ModuleRequestsRebuild},
      {"data_scale_scales_the_cap", DataScaleScalesTheCap},
      {"failed_build_releases_readers", FailedBuildReleasesReaders},
      {"invalidate_during_build", InvalidateDuringBuild},
      {"reader_before_first_commit", ReaderBeforeFirstCommit},
      {"reader_in_stale_window", ReaderInStaleWindow},
      {"host_destroyed_mid_commit", HostDestroyedMidCommit},
      {"uncaptured_commit", UncapturedCommit},
      {"shipped_commits", ShippedCommits},
      {"thread_limits_per_phase", ThreadLimitsPerPhase},
      {"maintenance_lookups", MaintenanceLookups},
  };
  int failed = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << "\n";
    } catch (const std::exception &e) {
      std::cout << "FAIL " << name << ": " << e.what() << "\n";
      failed++;
    }
  }
  return failed == 0 ? 0 : 1;
}
