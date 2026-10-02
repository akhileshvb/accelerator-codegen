// VersionedAuxStore under concurrent readers and writers.
//
//  1. Semantics: version selection by snapshot epoch, kAllSnapshots builds,
//     pruning, invalidation for every snapshot, one version per commit (an
//     update of one partition and an invalidation of another are seen
//     together or not at all), one commit per accelerator at a time, one
//     version per AuxReadScope.
//  2. Pending commits: readers whose snapshot may include the commit wait
//     for it; older snapshots and other accelerators read on; abort; timeout.
//  3. Coordinator: PRE_COMMIT marks pending (every partition it changes),
//     COMMIT publishes one version, ABORT keeps the current one.
//  4. Stress (in memory): one writer committing as fast as it can (random
//     partition updates, invalidations, rebuilds, aborts), many readers with
//     short and long snapshots, several reads per query; every value checked
//     against a model of every version.
//  5. DuckDB, end to end, on real transactions at a high commit rate: every
//     aux read equals count(*) in the same transaction.
//
// Usage: test_versioned_aux_state [stress_commits=200000] [readers=8]
//                                 [duckdb_commits=3000]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_registry.hpp"
#include "accelerator_codegen/stateful/core/aux_txn_buffer.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"
#include "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"
#include "accelerator_codegen/stateful/state/versioned_aux_store.hpp"
#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::core;
using Clock = std::chrono::steady_clock;
using Epoch = VersionedAuxStore::Epoch;

namespace {

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      throw std::runtime_error(std::string("CHECK failed: ") + #cond +         \
                               " at line " + std::to_string(__LINE__));       \
    }                                                                          \
  } while (0)

double MillisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// ---- small helpers ----

std::shared_ptr<const void> IntPayload(int v) { return std::make_shared<const int>(v); }

StateImage Image(const std::string &accelerator, PartitionId partition, int v) {
  StateImage image;
  image.accelerator = accelerator;
  image.partition = partition;
  image.payload = IntPayload(v);
  return image;
}

InvalidationPlan Parts(std::initializer_list<PartitionKey> keys) {
  InvalidationPlan plan;
  plan.scope = InvalidationScope::PartitionSet;
  plan.partitions.insert(keys.begin(), keys.end());
  return plan;
}

VisibilityToken At(Epoch e) {
  VisibilityToken t;
  t.mutation_epoch = e;
  return t;
}

// Value of `partition` at snapshot `e` (-1: absent / invalid / not visible).
int ValueAt(const VersionedAuxStore &store, Epoch e, PartitionId partition) {
  auto snapshot = store.ReadPartitions(At(e), {partition});
  if (snapshot.partitions.empty()) {
    return -1;
  }
  return *static_cast<const int *>(snapshot.partitions[0].payload.get());
}

struct EpochVar {
  std::atomic<Epoch> value {0};
  EpochVar &operator=(Epoch e) {
    value = e;
    return *this;
  }
  VersionedAuxStore::EpochProvider Provider() {
    return [this] { return value.load(); };
  }
};

VersionedAuxStore::Options Opts(EpochVar &epoch, std::function<Epoch()> floor = {}) {
  VersionedAuxStore::Options o;
  o.epoch = epoch.Provider();
  o.floor = std::move(floor);
  return o;
}

// ---- 1. semantics ----

void TestSemantics() {
  EpochVar epoch;
  Epoch floor = 0;
  VersionedAuxStore store(Opts(epoch, [&] { return floor; }));
  store.Install({Image("a", 1, 100), Image("a", 2, 200)}, "a", VersionedAuxStore::kAllSnapshots);
  CHECK(ValueAt(store, 7, 1) == 100 && ValueAt(store, 0, 2) == 200);

  // Commit at 10: update 1, invalidate 2 — one version.
  epoch = 10;
  store.BeginCommit("a", Parts({1, 2}));
  store.FinishCommit("a", Parts({2}), {Image("a", 1, 110)});
  CHECK(ValueAt(store, 5, 1) == 100 && ValueAt(store, 5, 2) == 200); // old snapshot: both old
  CHECK(ValueAt(store, 10, 1) == 110 && ValueAt(store, 10, 2) == -1); // new: both new
  CHECK(store.CheckValidity({"a", ""}, At(5)).code == ValidityCode::Valid);
  CHECK(store.CheckValidity({"a", ""}, At(10)).code == ValidityCode::Invalidated);
  CHECK(store.CheckValidity({"a", "1"}, At(10)).code == ValidityCode::Valid);

  // Rebuild at 20 brings 2 back.
  epoch = 20;
  store.BeginCommit("a", Parts({2}));
  store.FinishCommit("a", {}, {Image("a", 2, 220)});
  CHECK(store.CheckValidity({"a", ""}, At(25)).code == ValidityCode::Valid);
  CHECK(store.TotalVersionCount() == 3);

  // Invalidate (outside a commit): partition 1 unusable for every snapshot,
  // partition 2 still per-snapshot.
  store.Invalidate(Parts({1}));
  CHECK(ValueAt(store, 5, 1) == -1 && ValueAt(store, 25, 1) == -1);
  CHECK(ValueAt(store, 5, 2) == 200 && ValueAt(store, 25, 2) == 220);

  // Pruning: floor 20 keeps only the version a snapshot at 20 sees.
  floor = 20;
  epoch = 30;
  store.BeginCommit("a", Parts({1}));
  store.FinishCommit("a", {}, {Image("a", 1, 130)});
  CHECK(store.TotalVersionCount() == 2);
  CHECK(store.CheckValidity({"a", ""}, At(5)).code == ValidityCode::NotVisible);

  // kAllSnapshots install replaces the history.
  store.Install({Image("a", 1, 1)}, "a", VersionedAuxStore::kAllSnapshots);
  CHECK(store.TotalVersionCount() == 1 && ValueAt(store, 5, 1) == 1);

  // One commit per accelerator at a time; other accelerators independent.
  store.BeginCommit("a", Parts({1}));
  bool threw = false;
  try {
    store.BeginCommit("a", Parts({1}));
  } catch (const std::logic_error &) {
    threw = true;
  }
  CHECK(threw);
  store.BeginCommit("b", Parts({9}));
  store.AbortCommit("b");
  store.AbortCommit("a");

  // One version per read scope.
  epoch = 40;
  store.Install({Image("a", 1, 1), Image("a", 2, 2)}, "a", VersionedAuxStore::kAllSnapshots);
  {
    AuxReadScope scope;
    CHECK(ValueAt(store, 0, 1) == 1);
    store.BeginCommit("a", Parts({1, 2}));
    store.FinishCommit("a", {}, {Image("a", 1, 41), Image("a", 2, 42)});
    CHECK(ValueAt(store, 0, 2) == 2); // still the version the scope pinned
  }
  CHECK(ValueAt(store, 0, 2) == 42);

  // Store with no epoch provider: every version is kAllSnapshots.
  VersionedAuxStore plain(VersionedAuxStore::Options {});
  plain.Install({Image("x", 1, 5)}, "x");
  plain.BeginCommit("x", Parts({1}));
  plain.FinishCommit("x", {}, {Image("x", 1, 6)});
  CHECK(plain.TotalVersionCount() == 1 && ValueAt(plain, 0, 1) == 6 && ValueAt(plain, 3, 1) == 6);

  // A full rebuild (invalidate all + updates) replaces the state: a partition
  // the rebuild no longer has is dropped, not left invalid.
  epoch = 50;
  store.Install({Image("a", 1, 1), Image("a", 2, 2)}, "a", VersionedAuxStore::kAllSnapshots);
  InvalidationPlan all;
  all.scope = InvalidationScope::AllPartitions;
  store.BeginCommit("a", all);
  store.FinishCommit("a", all, {Image("a", 1, 51)});
  CHECK(store.CheckValidity({"a", ""}, At(50)).code == ValidityCode::Valid);
  CHECK(ValueAt(store, 50, 1) == 51 && ValueAt(store, 50, 2) == -1);
  CHECK(ValueAt(store, 45, 2) == 2); // older snapshots keep it
}

// ---- 2. pending ----

void TestPending() {
  EpochVar epoch;
  VersionedAuxStore store(Opts(epoch, [] { return Epoch(0); }));
  store.Install({Image("a", 1, 100)}, "a", 1);
  store.Install({Image("b", 7, 700)}, "b", 1);

  epoch = 10;
  store.BeginCommit("a", Parts({1}));
  auto old_snapshot = std::async(std::launch::async, [&] {
    const auto t = Clock::now();
    const auto value = ValueAt(store, 5, 1);
    return std::make_pair(value, MillisSince(t));
  });
  auto new_snapshot = std::async(std::launch::async, [&] {
    const auto t = Clock::now();
    const auto value = ValueAt(store, 12, 1);
    return std::make_pair(value, MillisSince(t));
  });
  auto newest = std::async(std::launch::async, [&] { return ValueAt(store, 0, 1); });
  auto other = std::async(std::launch::async, [&] {
    const auto t = Clock::now();
    const auto value = ValueAt(store, 12, 7);
    return std::make_pair(value, MillisSince(t));
  });
  const auto o = old_snapshot.get();
  CHECK(o.first == 100 && o.second < 50);
  const auto b = other.get();
  CHECK(b.first == 700 && b.second < 50); // another accelerator is not pending
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  epoch = 11;
  store.FinishCommit("a", {}, {Image("a", 1, 111)});
  const auto n = new_snapshot.get();
  CHECK(n.first == 111 && n.second >= 80);
  CHECK(newest.get() == 111);

  // Abort: waiters read the current state.
  epoch = 20;
  store.BeginCommit("a", Parts({1}));
  auto waiting = std::async(std::launch::async, [&] { return ValueAt(store, 25, 1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  store.AbortCommit("a");
  CHECK(waiting.get() == 111);

  // Timeout: not visible.
  store.SetPendingWaitMicros(20000);
  store.BeginCommit("a", Parts({1}));
  auto timed = std::async(std::launch::async, [&] {
    const auto t = Clock::now();
    const auto value = store.CheckValidity({"a", ""}, At(25)).code;
    return std::make_pair(value, MillisSince(t));
  });
  const auto tr = timed.get();
  CHECK(tr.first == ValidityCode::NotVisible && tr.second >= 15);
  store.AbortCommit("a");

  // kWaitForPublish: no timeout (beyond the 500 ms default).
  store.SetPendingWaitMicros(VersionedAuxStore::kWaitForPublish);
  epoch = 30;
  store.BeginCommit("a", Parts({1}));
  auto patient = std::async(std::launch::async, [&] {
    const auto t = Clock::now();
    const auto value = ValueAt(store, 35, 1);
    return std::make_pair(value, MillisSince(t));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  epoch = 31;
  store.FinishCommit("a", {}, {Image("a", 1, 131)});
  const auto pr = patient.get();
  CHECK(pr.first == 131 && pr.second >= 650);
}

// ---- 3. coordinator ----

// A module whose state is partition 1 = next; optionally invalidates 2.
class CounterModule final : public AuxQueryModule {
public:
  AcceleratorId Id() const override { return "counter"; }
  std::vector<TableDependency> Dependencies() const override { return {{7, {0}}}; }
  std::unique_ptr<AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<BasicAuxTxnBuffer>();
  }
  void Build(AuxEngineServices &, StateBuilder &builder) override {
    builds++;
    builder.Write(Image(Id(), 1, next));
    builder.Write(Image(Id(), 2, next + 1));
    builder.Finish();
  }
  InvalidationPlan InterpretMutation(const MutationBatch &) const override { return {}; }
  void AccumulateMutation(const MutationBatch &, AuxEngineServices &, AuxTxnBuffer &buffer) override {
    static_cast<BasicAuxTxnBuffer &>(buffer).MarkPending();
  }
  StagedMaintenanceOutcome StageMaintenance(AuxEngineServices &, const InvalidationPlan &,
                                            AuxTxnBuffer &) override {
    StagedMaintenanceOutcome outcome;
    outcome.action = MaintenanceAction::ApplyDelta;
    outcome.updates.push_back(Image(Id(), 1, next));
    if (invalidate_two) {
      outcome.invalidations = Parts({2});
    }
    return outcome;
  }
  bool CheckValidity(AuxEngineServices &, const QueryArguments &) const override { return true; }
  void Execute(AuxEngineServices &, const QueryArguments &, AuxRowSink &) override {}
  void ExecuteFallback(AuxEngineServices &, const QueryArguments &, AuxRowSink &) override {}

  int next = 0;
  bool invalidate_two = false;
  int builds = 0;
};

class Services final : public AuxEngineServices {
public:
  explicit Services(AuxStateStore &store_p, bool readers_fall_back_p = true)
      : store(store_p), readers_fall_back(readers_fall_back_p) {}
  VisibilityToken CurrentVisibility() const override { return {}; }
  bool ReadersCanBuildState() const override { return readers_fall_back; }
  bool readers_fall_back;
  AuxStateStore &StateStore() override { return store; }
  AuxStateStore &store;
};

void TestCoordinator() {
  EpochVar epoch;
  VersionedAuxStore store(Opts(epoch, [] { return Epoch(0); }));
  Services services(store);
  AuxTransactionCoordinator coordinator;
  auto module = std::make_shared<CounterModule>();
  coordinator.Register(module);
  coordinator.AttachServices(services);
  store.Install({Image("counter", 1, 100), Image("counter", 2, 200)}, "counter", 1);
  MutationBatch mutation;
  mutation.table = 7;

  // Updates partition 1 and invalidates 2: a reader of partition 1 alone,
  // whose snapshot includes the commit, still waits for it.
  module->next = 111;
  module->invalidate_two = true;
  coordinator.OnMutation(mutation);
  epoch = 10;
  coordinator.PreCommit();
  CHECK(store.HasPendingCommit());
  CHECK(ValueAt(store, 5, 1) == 100 && ValueAt(store, 5, 2) == 200); // not invalidated
  auto reader = std::async(std::launch::async, [&] { return ValueAt(store, 12, 1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  epoch = 11;
  coordinator.Commit();
  CHECK(reader.get() == 111);
  CHECK(ValueAt(store, 11, 2) == -1 && ValueAt(store, 5, 2) == 200);

  // Abort after PRE_COMMIT.
  module->next = 999;
  module->invalidate_two = false;
  coordinator.OnMutation(mutation);
  epoch = 20;
  coordinator.PreCommit();
  auto aborted = std::async(std::launch::async, [&] { return ValueAt(store, 25, 1); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  coordinator.Abort();
  CHECK(aborted.get() == 111);
  CHECK(!store.HasPendingCommit());

  // Readers that cannot fall back: an outcome leaving partition 2 invalidated
  // and not rebuilt becomes a full rebuild; the state stays valid.
  VersionedAuxStore strict_store(Opts(epoch, [] { return Epoch(0); }));
  Services strict(strict_store, /*readers_fall_back=*/false);
  AuxTransactionCoordinator strict_coordinator;
  auto strict_module = std::make_shared<CounterModule>();
  strict_coordinator.Register(strict_module);
  strict_coordinator.AttachServices(strict);
  strict_store.Install({Image("counter", 1, 100), Image("counter", 2, 200)}, "counter", 1);
  strict_module->next = 300;
  strict_module->invalidate_two = true;
  strict_coordinator.OnMutation(mutation);
  epoch = 30;
  strict_coordinator.PreCommit();
  epoch = 31;
  strict_coordinator.Commit();
  CHECK(strict_module->builds == 1);
  CHECK(strict_store.CheckValidity({"counter", ""}, At(31)).code == ValidityCode::Valid);
  CHECK(ValueAt(strict_store, 31, 1) == 300 && ValueAt(strict_store, 31, 2) == 301);
}

// ---- 4. in-memory stress ----

constexpr int kStressPartitions = 6;

struct Cell {
  uint64_t commit; // the commit that wrote it
  int partition;
};

// The model: per version, per partition, the writing commit (0: invalid).
struct Model {
  std::vector<std::array<uint64_t, kStressPartitions>> by_version;
};

void TestStress(uint64_t commits, int readers) {
  std::atomic<Epoch> visible {0}; // newest committed version
  std::atomic<Epoch> next_epoch {0};
  std::mutex registry_mutex;
  std::vector<Epoch> open(static_cast<std::size_t>(readers), 0); // 0: none
  VersionedAuxStore::Options o;
  o.epoch = [&] { return next_epoch.load(); };
  o.floor = [&] {
    std::lock_guard<std::mutex> l(registry_mutex);
    Epoch f = visible.load();
    for (auto e : open) {
      if (e != 0) {
        f = std::min(f, e);
      }
    }
    return f;
  };
  VersionedAuxStore store(o);

  // Version 1: all partitions written by "commit 1".
  Model model;
  model.by_version.resize(commits + 2);
  std::vector<StateImage> initial;
  for (int p = 0; p < kStressPartitions; p++) {
    initial.push_back(Image("s", p, 0));
    initial.back().payload = std::make_shared<const Cell>(Cell {1, p});
    model.by_version[1][static_cast<std::size_t>(p)] = 1;
  }
  store.Install(initial, "s", 1);
  visible = 1;

  std::atomic<bool> stop {false};
  std::atomic<long> reads {0}, checks {0}, waits {0}, not_visible {0}, invalid_reads {0};
  std::atomic<long> failures {0};
  std::mutex failure_mutex;
  std::string first_failure;
  auto fail = [&](const std::string &m) {
    std::lock_guard<std::mutex> l(failure_mutex);
    if (failures++ == 0) {
      first_failure = m;
    }
  };

  std::vector<std::thread> threads;
  for (int r = 0; r < readers; r++) {
    threads.emplace_back([&, r] {
      std::mt19937_64 rng(1000 + static_cast<uint64_t>(r));
      while (!stop.load()) {
        Epoch e;
        {
          std::lock_guard<std::mutex> l(registry_mutex);
          e = visible.load();
          open[static_cast<std::size_t>(r)] = e;
        }
        const bool long_txn = rng() % 32 == 0;
        const int queries = 1 + static_cast<int>(rng() % (long_txn ? 6 : 2));
        for (int q = 0; q < queries && !stop.load(); q++) {
          AuxReadScope scope;
          const auto &expected = model.by_version[e];
          bool all_valid = true;
          for (auto w : expected) {
            all_valid = all_valid && w != 0;
          }
          const int reads_in_query = 1 + static_cast<int>(rng() % 3);
          for (int k = 0; k < reads_in_query; k++) {
            const auto t = Clock::now();
            if (rng() % 3 == 0) {
              const auto v = store.CheckValidity({"s", ""}, At(e));
              if (v.code == ValidityCode::NotVisible) {
                not_visible++;
                fail("NotVisible for open snapshot " + std::to_string(e));
              } else if ((v.code == ValidityCode::Valid) != all_valid) {
                fail("validity mismatch at version " + std::to_string(e));
              }
            } else {
              std::vector<PartitionId> want;
              for (int p = 0; p < kStressPartitions; p++) {
                if (rng() % 2) {
                  want.push_back(static_cast<PartitionId>(p));
                }
              }
              const auto snap = store.ReadPartitions(At(e), want);
              std::map<PartitionId, uint64_t> got;
              for (const auto &img : snap.partitions) {
                got[img.partition] = static_cast<const Cell *>(img.payload.get())->commit;
              }
              for (auto p : want) {
                const auto exp = expected[static_cast<std::size_t>(p)];
                auto it = got.find(p);
                if (exp == 0) {
                  invalid_reads++;
                  if (it != got.end()) {
                    fail("read invalid partition at version " + std::to_string(e));
                  }
                } else if (it == got.end() || it->second != exp) {
                  fail("partition " + std::to_string(p) + " at version " + std::to_string(e) +
                       ": expected commit " + std::to_string(exp) + ", got " +
                       (it == got.end() ? std::string("none") : std::to_string(it->second)));
                }
                checks++;
              }
            }
            reads++;
            if (MillisSince(t) > 0.05) {
              waits++;
            }
          }
          if (long_txn) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
          }
        }
        std::lock_guard<std::mutex> l(registry_mutex);
        open[static_cast<std::size_t>(r)] = 0;
      }
    });
  }

  // Writer: as fast as it can.
  std::mt19937_64 rng(7);
  uint64_t committed = 1, aborted = 0, rebuilds = 0, invalidations = 0;
  std::size_t max_versions = 0;
  const auto start = Clock::now();
  for (uint64_t c = 2; c <= commits + 1; c++) {
    const Epoch v = committed + 1; // the version this commit creates
    next_epoch = v;                // pending from v
    InvalidationPlan touched;
    touched.scope = InvalidationScope::AllPartitions;
    store.BeginCommit("s", touched);
    if (rng() % 16 == 0) {
      store.AbortCommit("s");
      aborted++;
      continue;
    }
    auto state = model.by_version[committed];
    InvalidationPlan invalidate;
    std::vector<StateImage> updates;
    const auto kind = rng() % 20;
    if (kind == 0) { // rebuild everything
      invalidate.scope = InvalidationScope::AllPartitions;
      for (int p = 0; p < kStressPartitions; p++) {
        updates.push_back(Image("s", p, 0));
        updates.back().payload = std::make_shared<const Cell>(Cell {v, p});
        state[static_cast<std::size_t>(p)] = v;
      }
      rebuilds++;
    } else {
      for (int p = 0; p < kStressPartitions; p++) {
        if (rng() % 3 == 0 || state[static_cast<std::size_t>(p)] == 0) {
          updates.push_back(Image("s", p, 0));
          updates.back().payload = std::make_shared<const Cell>(Cell {v, p});
          state[static_cast<std::size_t>(p)] = v;
        }
      }
      if (kind == 1) { // invalidate one partition not updated
        const int p = static_cast<int>(rng() % kStressPartitions);
        bool updated = false;
        for (const auto &img : updates) {
          updated = updated || img.partition == static_cast<PartitionId>(p);
        }
        if (!updated) {
          invalidate = Parts({static_cast<PartitionKey>(p)});
          state[static_cast<std::size_t>(p)] = 0;
          invalidations++;
        }
      }
    }
    model.by_version[v] = state; // before anyone can see v
    visible = v;                  // the engine commit is visible
    if (rng() % 64 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(50)); // slow publish
    }
    store.FinishCommit("s", invalidate, updates);
    committed = v;
    max_versions = std::max(max_versions, store.TotalVersionCount());
  }
  const double secs = MillisSince(start) / 1000.0;
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  std::cout << "  stress: " << committed - 1 << " commits (" << aborted << " aborted, " << rebuilds
            << " rebuilds, " << invalidations << " invalidations) in " << secs << " s ("
            << static_cast<long>((committed - 1) / secs) << " commits/s); " << reads.load()
            << " reads, " << checks.load() << " partition checks (" << invalid_reads.load()
            << " of invalid partitions), " << waits.load() << " slow (>50us); max versions kept "
            << max_versions << std::endl;
  if (failures > 0) {
    throw std::runtime_error(std::to_string(failures.load()) + " stress failures; first: " + first_failure);
  }
  CHECK(reads > 0 && checks > 0);
  CHECK(not_visible == 0);
}

// ---- 5. DuckDB end to end ----

void Exec(duckdb::Connection &c, const std::string &sql) {
  auto r = c.Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error(sql + ": " + (r ? r->GetError() : std::string("null")));
  }
}

int64_t Count(duckdb::Connection &c) {
  auto r = c.Query("SELECT count(*) FROM t");
  if (!r || r->HasError()) {
    throw std::runtime_error("count failed: " + (r ? r->GetError() : std::string("null")));
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

// The aux value the connection's current transaction sees (-1: none).
int AuxFor(duckdb::Connection &reader, const AuxStateStore &store) {
  engine::duckdb::DuckDBAuxBackend::ReaderScope scope(*reader.context);
  const auto token = engine::duckdb::DuckDBAuxBackend::ReaderVisibility();
  const auto snapshot = store.ReadPartitions(token, {1});
  return snapshot.partitions.empty() ? -1 : *static_cast<const int *>(snapshot.partitions[0].payload.get());
}

void TestDuckDB(int commits, int readers) {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection writer(db);
  Exec(writer, "CREATE TABLE t(x INTEGER)");
  Exec(writer, "INSERT INTO t VALUES (0)");

  auto attached = engine::duckdb::AuxCommitClock::DefaultDatabase(*writer.context);
  auto clock = writer.context->registered_state->GetOrCreate<engine::duckdb::AuxCommitClock>(
      "accel_aux_commit_clock", attached);
  engine::duckdb::DuckDBStateStore store(
      "counter", [clock] { return clock->epoch.load(); },
      [attached]() -> Epoch {
        return attached->GetTransactionManager().Cast<duckdb::DuckTransactionManager>().LowestActiveStart();
      });
  Services services(store);
  AuxTransactionCoordinator coordinator;
  auto module = std::make_shared<CounterModule>();
  coordinator.Register(module);
  coordinator.AttachServices(services);
  writer.BeginTransaction(); // a build is current from a fresh snapshot's start time
  const Epoch build_epoch = engine::duckdb::DuckDBAuxBackend::SnapshotEpoch(*writer.context);
  writer.Rollback();
  store.Install({Image("counter", 1, 1)}, "counter", build_epoch); // count = 1

  std::atomic<bool> stop {false};
  std::atomic<long> reads {0}, mismatches {0}, misses {0}, long_txns {0};
  std::mutex failure_mutex;
  std::string first_failure;
  std::vector<std::thread> threads;
  for (int r = 0; r < readers; r++) {
    threads.emplace_back([&, r] {
      duckdb::Connection conn(db);
      std::mt19937_64 rng(77 + static_cast<uint64_t>(r));
      while (!stop.load()) {
        try {
          Exec(conn, "BEGIN TRANSACTION");
          const bool long_txn = rng() % 16 == 0;
          const int n = long_txn ? 4 : 1;
          for (int k = 0; k < n; k++) {
            const auto sql = Count(conn);
            const auto aux = AuxFor(conn, store);
            reads++;
            if (aux == -1) {
              misses++;
            } else if (aux != sql) {
              mismatches++;
              std::lock_guard<std::mutex> l(failure_mutex);
              if (first_failure.empty()) {
                engine::duckdb::DuckDBAuxBackend::ReaderScope scope(*conn.context);
                first_failure = "count " + std::to_string(sql) + " but aux " + std::to_string(aux) +
                                " (reader epoch " +
                                std::to_string(engine::duckdb::DuckDBAuxBackend::ReaderVisibility().mutation_epoch) +
                                ", clock " + std::to_string(clock->epoch.load()) + ")";
              }
            }
            if (long_txn) {
              std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
          }
          long_txns += long_txn ? 1 : 0;
          Exec(conn, "COMMIT");
        } catch (const std::exception &e) {
          std::lock_guard<std::mutex> l(failure_mutex);
          if (first_failure.empty()) {
            first_failure = e.what();
          }
          mismatches++;
          return;
        }
      }
    });
  }

  MutationBatch mutation;
  mutation.table = 7;
  module->invalidate_two = false;
  const auto start = Clock::now();
  int count = 1, aborts = 0;
  std::mt19937_64 rng(5);
  for (int i = 0; i < commits; i++) {
    Exec(writer, "BEGIN TRANSACTION");
    Exec(writer, "INSERT INTO t VALUES (" + std::to_string(i) + ")");
    coordinator.OnMutation(mutation);
    module->next = count + 1;
    clock->epoch = engine::duckdb::DuckDBAuxBackend::SnapshotEpoch(*writer.context) + 1;
    coordinator.PreCommit();
    if (rng() % 20 == 0) {
      Exec(writer, "ROLLBACK");
      coordinator.Abort();
      aborts++;
      continue;
    }
    Exec(writer, "COMMIT");
    if (rng() % 50 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200)); // slow publish
    }
    coordinator.Commit();
    count++;
  }
  const double secs = MillisSince(start) / 1000.0;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  std::cout << "  duckdb: " << commits - aborts << " commits (" << aborts << " aborted) in " << secs
            << " s (" << static_cast<long>((commits - aborts) / secs) << " commits/s); " << reads.load()
            << " aux reads (" << long_txns.load() << " long transactions), " << misses.load()
            << " misses, " << mismatches.load() << " mismatches; versions kept "
            << store.TotalVersionCount() << std::endl;
  if (mismatches > 0) {
    throw std::runtime_error("duckdb mismatches; first: " + first_failure);
  }
  CHECK(reads > 0 && misses == 0);
  duckdb::Connection after(db);
  Exec(after, "BEGIN TRANSACTION");
  CHECK(Count(after) == count && AuxFor(after, store) == count);
  Exec(after, "COMMIT");
}

} // namespace

int main(int argc, char **argv) {
  const uint64_t stress_commits = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  const int readers = argc > 2 ? std::atoi(argv[2]) : 8;
  const int duckdb_commits = argc > 3 ? std::atoi(argv[3]) : 3000;
  try {
    TestSemantics();
    std::cout << "PASS semantics" << std::endl;
    TestPending();
    std::cout << "PASS pending commits" << std::endl;
    TestCoordinator();
    std::cout << "PASS coordinator" << std::endl;
    TestStress(stress_commits, readers);
    std::cout << "PASS in-memory stress" << std::endl;
    if (duckdb_commits > 0) {
      TestDuckDB(duckdb_commits, readers);
      std::cout << "PASS duckdb end to end" << std::endl;
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "test_versioned_aux_state failed: " << e.what() << std::endl;
    return 1;
  }
}
