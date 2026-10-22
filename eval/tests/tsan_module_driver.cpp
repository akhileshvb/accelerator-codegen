// Engine-free stress of one aux module under the runtime host, for a ThreadSanitizer build.
//
//   tsan_module_driver <module> [seconds=5] [writers=4] [readers=4]
//
// Concurrent writers (capture, pre-commit, commit), readers at the newest and at
// past epochs, and maintenance/build/read threads at ACCEL_AUX_{READ,BUILD,MAINT}_THREADS,
// against a fake engine (a list of committed deltas). Exit 0 clean, 1 on a wrong
// read, 77 when `module` has no engine-free driver case (see kCases).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "substrate/aux/core/aux_txn_buffer.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::runtime;

namespace {

constexpr core::TableId kTable = 7;

struct FakeDb {
  Epoch Commit(int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex);
    commits.emplace_back(clock += 7, delta);
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
      sum += e <= epoch ? delta : 0;
    }
    return sum;
  }
  std::mutex mutex;
  std::vector<std::pair<Epoch, int64_t>> commits;
  Epoch clock = 100;
};

// ---- the case: a module, how the fake engine builds it, how writers change it ----

struct SumBuffer final : core::AuxTxnBuffer {
  bool HasPendingChanges() const override { return pending; }
  void Clear() override { *this = SumBuffer(); }
  int64_t delta = 0;
  bool pending = false;
};

// SELECT sum(x): one Int64 state image.
struct SumModule final : core::AuxQueryModule {
  core::AcceleratorId Id() const override { return "sum"; }
  std::vector<core::TableDependency> Dependencies() const override { return {{kTable, {0}, {0}}}; }
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
    for (const auto &row : batch.new_rows) {
      sum.delta += std::get<int64_t>(row.values[0].payload);
    }
    sum.pending = true;
  }
  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &services,
                                                  const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &buffer) override {
    const auto current = services.StateStore().Read(services.CurrentVisibility());
    core::StagedMaintenanceOutcome outcome;
    outcome.action = core::MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(core::StateImage::Make<int64_t>(
        Id(), 0, 0,
        std::make_shared<const int64_t>(*current.partitions.at(0).As<int64_t>() +
                                        static_cast<SumBuffer &>(buffer).delta)));
    return outcome;
  }
  bool CheckValidity(core::AuxEngineServices &services, const core::QueryArguments &) const override {
    return services.StateStore().CheckValidity(core::QueryKey{Id(), ""}, services.CurrentVisibility()).IsValid();
  }
  void Execute(core::AuxEngineServices &services, const core::QueryArguments &,
               core::AuxRowSink &sink) override {
    const auto state = services.StateStore().Read(services.CurrentVisibility());
    core::AuxRow row;
    row.values.push_back(core::AuxValue::Of(core::AuxType::Int64, *state.partitions.at(0).As<int64_t>()));
    sink.WriteRow(row);
  }
  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &, core::AuxRowSink &) override {}
};

ColumnarBatchRef InsertBatch(int64_t value) {
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

struct Slots final : EngineSlots {
  explicit Slots(FakeDb &db) : db(db) {}
  Epoch PendingEpochLowerBound(uint64_t) const override { return db.Now() + 1; }
  Epoch OldestReaderEpoch() const override { return 1; } // keeps every version
  bool WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) override { return true; }
  std::unique_ptr<BuildSnapshot> OpenBuildSnapshot(AcceleratorRuntime &) override {
    struct Snapshot final : BuildSnapshot {
      explicit Snapshot(Epoch e) : epoch(e) {}
      Epoch SnapshotEpoch() const override { return epoch; }
      Epoch epoch;
    };
    return std::make_unique<Snapshot>(db.Now());
  }
  bool EngineWorkOnHostThread() const override { return false; }
  void RunBuild(AcceleratorRuntime &runtime, BuildSnapshot &snapshot, core::StateBuilder &builder) override {
    builder.Write(core::StateImage::Make<int64_t>(
        runtime.Id(), 0, 0, std::make_shared<const int64_t>(db.SumAt(snapshot.SnapshotEpoch()))));
    builder.Finish();
  }
  const maintenance::CostModelParams &CostModel(const core::AcceleratorId &) const override { return params; }
  FakeDb &db;
  maintenance::CostModelParams params = [] {
    maintenance::CostModelParams p;
    p.fallback_us = 60e6; // readers wait for the state rather than fall back
    return p;
  }();
};

struct CollectSink final : core::AuxRowSink {
  void Begin(const core::OutputSchema &) override {}
  void WriteRow(const core::AuxRow &row) override { values.push_back(std::get<int64_t>(row.values[0].payload)); }
  void End() override {}
  std::vector<int64_t> values;
};

struct Case {
  const char *name;
  std::shared_ptr<core::AuxQueryModule> (*make)();
};
const Case kCases[] = {{"sum", [] { return std::shared_ptr<core::AuxQueryModule>(std::make_shared<SumModule>()); }}};

int Stress(const Case &c, int seconds, int writers, int readers) {
  FakeDb db;
  Slots slots(db);
  RuntimeHost host(slots, ResolveThreadBudget(0));
  auto &threads = host.Threads();
  threads.SetPhaseLimits(ResolveReadThreads(0, threads.Budget()), ResolveBuildThreads(0, threads.Budget()),
                         ResolveMaintenanceThreads(0, threads.Budget()));
  auto &runtime = host.Register(c.make());
  host.BuildAll();

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> next_txn{1}, wrong{0}, served{0}, fell_back{0};
  std::vector<std::thread> pool;
  for (int w = 0; w < writers; ++w) {
    pool.emplace_back([&] {
      while (!stop) {
        const uint64_t txn = next_txn++;
        const int64_t delta = static_cast<int64_t>(txn % 13) + 1;
        host.OnCapture(txn, 0, InsertBatch(delta));
        host.PreCommit(txn);
        if (txn % 11 == 0) {
          host.Abort(txn);
          continue;
        }
        host.Commit(txn, db.Commit(delta));
      }
    });
  }
  for (int r = 0; r < readers; ++r) {
    pool.emplace_back([&, r] {
      ReadOptions options;
      options.max_wait_us = 2'000'000;
      while (!stop) {
        const Epoch now = db.Now();
        options.epoch = r % 2 ? now : now - 7 * (r + 1); // newest-ish and past epochs
        CollectSink sink;
        if (runtime.Read(options, {}, sink) == ReadResult::Fallback) {
          ++fell_back;
        } else if (sink.values.size() != 1 || sink.values[0] != db.SumAt(options.epoch)) {
          ++wrong;
        } else {
          ++served;
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop = true;
  for (auto &t : pool) {
    t.join();
  }
  std::cout << c.name << ": served=" << served << " fallback=" << fell_back << " wrong=" << wrong
            << " commits=" << runtime.Stats().commits_applied << "\n";
  return wrong ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: tsan_module_driver <module> [seconds] [writers] [readers]\n";
    return 2;
  }
  const auto arg = [&](int i, int fallback) { return argc > i ? std::atoi(argv[i]) : fallback; };
  for (const auto &c : kCases) {
    if (std::string(argv[1]) == c.name) {
      return Stress(c, arg(2, 5), arg(3, 4), arg(4, 4));
    }
  }
  std::cerr << argv[1] << ": no engine-free driver case. A module is fed by its query's InitialScan and "
            << "its writes need the module's own columns, both supplied by the engine's registration.\n";
  return 77;
}
