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
#include <atomic>
#include <chrono>
#include <condition_variable>
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
  Epoch OldestReaderEpoch() const override { return 1; } // keep every version
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) override {
    return true;
  }
  std::unique_ptr<BuildSnapshot> OpenBuildSnapshot(AcceleratorRuntime &) override {
    if (fail_builds) {
      throw std::runtime_error("no build snapshot");
    }
    return std::make_unique<FakeSnapshot>(db.Now());
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
    const int64_t sum = db.SumAt(snapshot.SnapshotEpoch());
    builder.Write(core::StateImage::Make<int64_t>(runtime.Id(), 0, 0,
                                                  std::make_shared<const int64_t>(sum)));
    builder.Finish();
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override {
    return params;
  }

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

} // namespace

int main() {
  const std::vector<std::pair<const char *, void (*)()>> tests = {
      {"in_process_commits", InProcessCommits},
      {"commits_during_build", CommitsDuringBuild},
      {"module_requests_rebuild", ModuleRequestsRebuild},
      {"failed_build_releases_readers", FailedBuildReleasesReaders},
      {"invalidate_during_build", InvalidateDuringBuild},
      {"shipped_commits", ShippedCommits},
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
