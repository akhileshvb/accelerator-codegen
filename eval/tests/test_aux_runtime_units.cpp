// Unit tests of the engine-neutral aux runtime pieces: ColumnarBatch,
// SequencedApplier and ThreadBudget.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "substrate/aux/core/aux_types.hpp"
#include "substrate/aux/runtime/columnar_batch.hpp"
#include "substrate/aux/runtime/sequenced_applier.hpp"
#include "substrate/aux/runtime/thread_budget.hpp"

using namespace accelerator_codegen::aux_state;
using namespace accelerator_codegen::aux_state::runtime;
using core::AuxRow;
using core::AuxType;
using core::AuxValue;

namespace {

#define CHECK(cond)                                                                         \
  do {                                                                                      \
    if (!(cond)) {                                                                          \
      throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) +       \
                               ": CHECK failed: " #cond);                                   \
    }                                                                                       \
  } while (0)

template <typename F> bool Throws(F &&f) {
  try {
    f();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

bool Same(const AuxValue &a, const AuxValue &b) {
  if (a.is_null || b.is_null) return a.is_null == b.is_null;   // an all-NULL column loses its type
  return a.type == b.type && a.payload == b.payload;
}

bool Same(const core::MutationBatch &a, const core::MutationBatch &b) {
  auto rows = [](const std::vector<AuxRow> &x, const std::vector<AuxRow> &y) {
    if (x.size() != y.size()) {
      return false;
    }
    for (std::size_t r = 0; r < x.size(); ++r) {
      if (x[r].values.size() != y[r].values.size()) {
        return false;
      }
      for (std::size_t c = 0; c < x[r].values.size(); ++c) {
        if (!Same(x[r].values[c], y[r].values[c])) {
          return false;
        }
      }
    }
    return true;
  };
  return a.table == b.table && a.kind == b.kind && rows(a.old_rows, b.old_rows) &&
         rows(a.new_rows, b.new_rows) && a.updated_columns == b.updated_columns;
}

// ---- ColumnarBatch --------------------------------------------------------

// One row per index i: a value of each type; NULL in column t when i % 3 == t % 3.
AuxRow MixedRow(int i) {
  const AuxType types[] = {AuxType::Boolean, AuxType::Int64,  AuxType::UInt64, AuxType::ScaledDecimal,
                           AuxType::Double,  AuxType::Date,   AuxType::String};
  AuxRow row;
  for (int t = 0; t < 7; ++t) {
    if (i % 3 == t % 3 && i % 2 == 0) {
      row.values.push_back(AuxValue::Null(types[t]));
      continue;
    }
    switch (types[t]) {
    case AuxType::Boolean: row.values.push_back(AuxValue::Of<bool>(types[t], i % 2 == 1)); break;
    case AuxType::Int64: row.values.push_back(AuxValue::Of<int64_t>(types[t], -i * 1000003LL)); break;
    case AuxType::UInt64: row.values.push_back(AuxValue::Of<uint64_t>(types[t], i * 7ULL)); break;
    case AuxType::ScaledDecimal:
      row.values.push_back(AuxValue::Of<core::ScaledDecimal>(types[t], {i * 25LL, 2}));
      break;
    case AuxType::Double: row.values.push_back(AuxValue::Of<double>(types[t], i * 0.5)); break;
    case AuxType::Date: row.values.push_back(AuxValue::Of<core::AuxDate>(types[t], {i - 100})); break;
    case AuxType::String:
      row.values.push_back(AuxValue::Of<std::string>(types[t], std::string(i % 5, 'a' + i % 26)));
      break;
    }
  }
  return row;
}

const std::vector<int32_t> kAttnums = {0, 1, 2, 3, 4, 5, 6};

core::MutationBatch MixedBatch(core::MutationKind kind, int rows) {
  core::MutationBatch b;
  b.table = 42;
  b.kind = kind;
  for (int i = 0; i < rows; ++i) {
    if (kind != core::MutationKind::Insert) b.old_rows.push_back(MixedRow(i));
    if (kind != core::MutationKind::Delete) b.new_rows.push_back(MixedRow(i + 1));
  }
  if (kind == core::MutationKind::Update) b.updated_columns = {1, 4};
  return b;
}

void TestBatchRoundTrip() {
  for (auto kind : {core::MutationKind::Insert, core::MutationKind::Delete, core::MutationKind::Update}) {
    for (int rows : {0, 1, 2, 17}) {
      auto legacy = MixedBatch(kind, rows);
      auto columnar = FromMutationBatch(legacy, kAttnums);
      CHECK(columnar.row_count == static_cast<std::size_t>(rows));
      CHECK(Same(ToMutationBatch(columnar, {}), legacy));
      CHECK(Same(ToMutationBatch(columnar, kAttnums), legacy));
    }
  }
}

void TestBatchTypesAndCells() {
  auto columnar = FromMutationBatch(MixedBatch(core::MutationKind::Insert, 6), kAttnums);
  const AuxType expected[] = {AuxType::Boolean, AuxType::Int64, AuxType::UInt64, AuxType::ScaledDecimal,
                              AuxType::Double,  AuxType::Date,  AuxType::String};
  for (int c = 0; c < 7; ++c) {
    CHECK(columnar.new_rows.columns[c].type == expected[c]);
  }
  CHECK(columnar.new_rows.columns[3].scale == 2);
  CHECK(columnar.new_rows.Find(5) == 5 && columnar.new_rows.Find(99) == -1);
  CHECK(Same(columnar.new_rows.columns[1].Value(1), MixedRow(1 + 1).values[1]));
  CHECK(columnar.new_rows.columns[0].IsNull(0) == MixedRow(1).values[0].is_null);
  CHECK(ByteSize(columnar) > 0);
}

void TestBatchProjection() {
  auto legacy = MixedBatch(core::MutationKind::Update, 5);
  auto columnar = FromMutationBatch(legacy, kAttnums);
  CHECK(columnar.updated_attnums == (std::vector<int32_t>{1, 4}));

  auto projected = ToMutationBatch(columnar, {4, 1, 6});
  CHECK(projected.updated_columns == (std::vector<core::ColumnId>{1, 0}));
  for (std::size_t r = 0; r < 5; ++r) {
    CHECK(Same(projected.new_rows[r].values[0], legacy.new_rows[r].values[4]));
    CHECK(Same(projected.old_rows[r].values[2], legacy.old_rows[r].values[6]));
  }
  // Updated attnums outside the projection report position projection.size().
  CHECK(ToMutationBatch(columnar, {0, 1}).updated_columns == (std::vector<core::ColumnId>{1, 2}));
  CHECK(Throws([&] { ToMutationBatch(columnar, {1, 99}); }));

  legacy.updated_columns.clear();
  CHECK(ToMutationBatch(FromMutationBatch(legacy, kAttnums), {}).updated_columns.empty());

  // Empty projection: ascending attnum order regardless of capture order.
  auto reordered = FromMutationBatch(MixedBatch(core::MutationKind::Insert, 3), {6, 2, 4, 0, 1, 3, 5});
  auto all = ToMutationBatch(reordered, {});
  CHECK(all.new_rows[0].values.size() == 7);
  CHECK(Same(all.new_rows[1].values[6], MixedRow(2).values[0]));
}

void TestBatchFromMutationErrors() {
  auto mk = [](std::vector<AuxValue> v) {
    core::MutationBatch b;
    for (auto &x : v) b.new_rows.push_back(AuxRow{{std::move(x)}});
    return b;
  };
  using core::ScaledDecimal;
  CHECK(Throws([&] {
    FromMutationBatch(mk({AuxValue::Of<ScaledDecimal>(AuxType::ScaledDecimal, {1, 2}),
                          AuxValue::Of<ScaledDecimal>(AuxType::ScaledDecimal, {1, 3})}), {0});
  }));
  auto all_null = FromMutationBatch(mk({AuxValue::Null(AuxType::Int64), AuxValue::Null(AuxType::Int64)}), {0});
  CHECK(all_null.new_rows.columns[0].type == AuxType::Int64); // the NULLs' declared type
  CHECK(ToMutationBatch(all_null, {}).new_rows[1].values[0].is_null);
  // Scale comes from the first non-NULL value, past leading NULLs.
  auto lead = FromMutationBatch(
      mk({AuxValue::Null(AuxType::ScaledDecimal), AuxValue::Of<ScaledDecimal>(AuxType::ScaledDecimal, {5, 4})}), {0});
  CHECK(lead.new_rows.columns[0].scale == 4);
}

void TestBatchLarge() {
  const std::size_t n = 1000000;
  core::MutationBatch legacy;
  legacy.kind = core::MutationKind::Insert;
  legacy.new_rows.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    legacy.new_rows.push_back(AuxRow{{AuxValue::Of<int64_t>(AuxType::Int64, static_cast<int64_t>(i)),
                                      AuxValue::Of<double>(AuxType::Double, i * 0.25),
                                      AuxValue::Of<core::AuxDate>(AuxType::Date, {static_cast<int32_t>(i % 20000)})}});
  }
  auto t0 = std::chrono::steady_clock::now();
  auto columnar = FromMutationBatch(legacy, {0, 1, 2});
  auto back = ToMutationBatch(columnar, {2, 0});
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "  1e6-row round trip: " << secs << " s\n";
  CHECK(back.new_rows.size() == n && Same(back.new_rows[n - 1].values[1], legacy.new_rows[n - 1].values[0]));
  CHECK(secs < 10.0);
}

// ---- SequencedApplier -----------------------------------------------------

CommittedChange Change(Seq seq, Epoch epoch, bool empty = false) {
  CommittedChange c;
  c.seq = seq;
  c.epoch = epoch;
  c.empty_commit = empty;
  return c;
}

std::vector<Seq> Seqs(const std::vector<CommittedChange> &v) {
  std::vector<Seq> out;
  for (const auto &c : v) out.push_back(c.seq);
  return out;
}
using SeqList = std::vector<Seq>;

SequencedApplier SyncedAt(Seq seq) {
  SequencedApplier a;
  a.StartBuild();
  a.TakeCatchUp(0);
  a.FinishCatchUp();
  for (Seq s = 1; s <= seq; ++s) a.MarkApplied(Change(s, s));
  return a;
}

void ApplyAll(SequencedApplier &a, const std::vector<CommittedChange> &changes) {
  for (const auto &c : changes) a.MarkApplied(c);
}

void TestApplierOrdering() {
  auto a = SyncedAt(0);
  CHECK(a.CurrentMode() == SequencedApplier::Mode::Synced);
  CHECK(a.Offer(Change(1, 1)) && a.TakeReady().size() == 1);
  CHECK(a.TakeReady().empty());
  // Out of order: 4, 2, 3 arrive before 1.
  CHECK(a.Offer(Change(4, 4)) && a.Offer(Change(2, 2)) && a.Offer(Change(3, 3)));
  CHECK(a.TakeReady().empty() && a.HasGap());
  CHECK(a.Offer(Change(1, 1)) && !a.HasGap());
  CHECK(Seqs(a.TakeReady()) == (SeqList{1, 2, 3, 4}));
}

void TestApplierDuplicatesAndGaps() {
  auto a = SyncedAt(0);
  CHECK(a.Offer(Change(2, 2)));
  CHECK(!a.Offer(Change(2, 2)));
  CHECK(a.HighestSeenSeq() == 2 && a.QueuedCount() == 1);
  CHECK(a.HasGap() && a.TakeReady().empty());
  CHECK(a.Offer(Change(1, 1)));
  CHECK(!a.HasGap());
  auto ready = a.TakeReady();
  CHECK(Seqs(ready) == (SeqList{1, 2}));
  ApplyAll(a, ready);
  CHECK(a.AppliedSeq() == 2 && a.AppliedEpoch() == 2);
  CHECK(!a.Offer(Change(1, 1)) && !a.Offer(Change(2, 2)));   // already applied
  CHECK(!a.HasGap());

  // Announce without Offer is a gap.
  a.Announce(4);
  CHECK(a.HighestSeenSeq() == 4 && a.HasGap());
  a.Offer(Change(3, 3));
  CHECK(!a.HasGap());
  a.Announce(2);
  CHECK(a.HighestSeenSeq() == 4);
}

void TestApplierEmptyCommits() {
  auto a = SyncedAt(0);
  a.Offer(Change(2, 5));
  a.Offer(Change(1, 0, true));
  auto ready = a.TakeReady();
  CHECK(Seqs(ready) == (SeqList{1, 2}) && ready[0].empty_commit);
  ApplyAll(a, ready);
  CHECK(a.AppliedEpoch() == 5);
  a.Offer(Change(3, 0, true));
  ApplyAll(a, a.TakeReady());
  CHECK(a.AppliedSeq() == 3 && a.AppliedEpoch() == 5);
}

// Build at epoch b with `epochs[i]` the epoch of seq i+1; offers `offered`
// (1-based seqs) before the catch-up and returns what TakeCatchUp gives.
struct BuildResult {
  SeqList returned;
  SequencedApplier applier;
};
BuildResult RunBuild(const std::vector<Epoch> &epochs, const SeqList &offered, Epoch b) {
  SequencedApplier a;
  a.StartBuild();
  CHECK(a.CurrentMode() == SequencedApplier::Mode::Building);
  a.SetBuildEpoch(b);
  for (Seq s : offered) a.Offer(Change(s, epochs[s - 1]));
  CHECK(a.TakeReady().empty());   // not Synced: nothing ready while building
  auto returned = Seqs(a.TakeCatchUp(b));
  return {returned, std::move(a)};
}

void TestApplierBuildDenseEpochs() {
  const std::vector<Epoch> dense = {1, 2, 3, 4, 5, 6};
  struct Case { SeqList offered; Epoch b; SeqList returned; Seq synced_at; bool gap; };
  const Case cases[] = {
      {{1, 2, 3, 4, 5, 6}, 3, {4, 5, 6}, 6, false},  // changes before and after b
      {{1, 2, 3}, 3, {}, 3, false},                   // all in the build
      {{1, 2, 3, 4, 5, 6}, 0, {1, 2, 3, 4, 5, 6}, 6, false},
      {{1, 2, 3, 5, 6}, 3, {}, 3, true},              // gap right after b
      {{1, 2, 3, 4, 6}, 3, {4}, 4, true},             // gap later
      {{4, 5}, 3, {}, 0, true},                       // 1..3 never arrived
  };
  for (const auto &c : cases) {
    auto r = RunBuild(dense, c.offered, c.b);
    CHECK(r.returned == c.returned);
    r.applier.FinishCatchUp();
    CHECK(r.applier.CurrentMode() == SequencedApplier::Mode::Synced);
    CHECK(r.applier.AppliedSeq() == c.synced_at);
    CHECK(r.applier.HasGap() == c.gap);
  }
}

void TestApplierBuildSparseEpochs() {
  const std::vector<Epoch> sparse = {101, 205, 309, 412, 520};
  auto r = RunBuild(sparse, {1, 2, 3, 4, 5}, 250);
  CHECK(r.returned == (SeqList{3, 4, 5}));
  r.applier.FinishCatchUp();
  CHECK(r.applier.AppliedSeq() == 5 && r.applier.AppliedEpoch() == 520);

  auto none = RunBuild(sparse, {1, 2}, 250);
  CHECK(none.returned.empty());
  none.applier.FinishCatchUp();
  CHECK(none.applier.AppliedSeq() == 2 && none.applier.AppliedEpoch() == 250);
}

void TestApplierCatchUpGapResume() {
  const std::vector<Epoch> dense = {1, 2, 3, 4, 5, 6, 7};
  auto r = RunBuild(dense, {1, 2, 3, 5, 6}, 2);
  CHECK(r.returned == (SeqList{3}));
  auto &a = r.applier;
  a.FinishCatchUp();
  CHECK(a.AppliedSeq() == 3 && a.HasGap() && a.QueuedCount() == 2);
  CHECK(a.TakeReady().empty());
  a.Offer(Change(4, 4));
  a.Offer(Change(7, 7));
  auto ready = a.TakeReady();
  CHECK(Seqs(ready) == (SeqList{4, 5, 6, 7}));
  ApplyAll(a, ready);
  CHECK(a.AppliedSeq() == 7 && a.QueuedCount() == 0);
}

void TestApplierEmptyCommitsInBuild() {
  SequencedApplier a;
  a.StartBuild();
  a.SetBuildEpoch(3);
  a.Offer(Change(2, 0, true));   // below the first kept change: in the build
  a.Offer(Change(3, 3));
  a.Offer(Change(4, 4));
  a.Offer(Change(5, 0, true));   // after it: returned
  CHECK(Seqs(a.TakeCatchUp(3)) == (SeqList{4, 5}));
  a.FinishCatchUp();
  CHECK(a.AppliedSeq() == 5 && a.QueuedCount() == 0);
}

void TestApplierModes() {
  SequencedApplier a;
  CHECK(a.CurrentMode() == SequencedApplier::Mode::Stale);
  a.Offer(Change(1, 1));
  CHECK(a.TakeReady().empty() && a.QueuedCount() == 1);
  CHECK(!a.HasGap());                 // seq 1 is queued
  a.Offer(Change(3, 3));
  a.StartBuild();
  CHECK(!a.HasGap());                 // building: not a gap yet
  a.AbandonBuild();
  CHECK(a.CurrentMode() == SequencedApplier::Mode::Stale && a.QueuedCount() == 2);

  auto s = SyncedAt(2);
  s.Invalidate();
  CHECK(s.CurrentMode() == SequencedApplier::Mode::Stale && s.AppliedSeq() == 2);
  s.Offer(Change(3, 3));
  s.StartBuild();
  CHECK(Seqs(s.TakeCatchUp(2)) == (SeqList{3}));
  s.FinishCatchUp();
  CHECK(s.CurrentMode() == SequencedApplier::Mode::Synced && s.AppliedSeq() == 3);

  CHECK(a.QueuedBytes() == 0);
  auto batch = std::make_shared<ColumnarBatch>(FromMutationBatch(MixedBatch(core::MutationKind::Insert, 4), kAttnums));
  CommittedChange withBatch = Change(1, 1);
  withBatch.batches = {batch, batch};
  SequencedApplier q;
  q.Offer(withBatch);
  CHECK(q.QueuedBytes() == 2 * ByteSize(*batch));
}

// ---- ThreadBudget ---------------------------------------------------------

class FakeTokens : public TokenSource {
public:
  explicit FakeTokens(std::size_t n) : available(n) {}
  std::size_t TryAcquire(std::size_t wanted) override {
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t got = std::min(wanted, available);
    available -= got;
    return got;
  }
  void Release(std::size_t count) override {
    std::lock_guard<std::mutex> lock(mutex);
    available += count;
  }
  std::size_t Available() {
    std::lock_guard<std::mutex> lock(mutex);
    return available;
  }

private:
  std::mutex mutex;
  std::size_t available;
};

// Tracks the number of tasks running at once.
struct Concurrency {
  std::atomic<int> now{0}, max{0};
  void Enter() {
    int v = ++now;
    int m = max.load();
    while (v > m && !max.compare_exchange_weak(m, v)) {}
  }
  void Leave() { --now; }
};

void SleepBriefly() { std::this_thread::sleep_for(std::chrono::microseconds(300)); }

void TestParallelForCoverage() {
  for (std::size_t budget : {1, 2, 4, 8}) {
    ThreadBudget pool(budget);
    for (std::size_t n : {0, 1, 3, 1000}) {
      std::vector<std::atomic<int>> hits(n);
      pool.ParallelFor(n, [&](std::size_t i) { ++hits[i]; });
      for (auto &h : hits) CHECK(h == 1);
    }
  }
  ThreadBudget pool(4);
  Concurrency conc;
  pool.ParallelFor(64, [&](std::size_t) { conc.Enter(); SleepBriefly(); conc.Leave(); }, 2);
  CHECK(conc.max <= 2);

  // Budget 1 runs on the calling thread.
  ThreadBudget one(1);
  const auto self = std::this_thread::get_id();
  one.ParallelFor(10, [&](std::size_t) { CHECK(std::this_thread::get_id() == self); });
  CHECK(one.PeakThreads() == 1);
}

void TestConcurrentCallersStayInBudget() {
  const std::size_t budget = 6;
  ThreadBudget pool(budget);
  Concurrency conc;
  std::vector<std::thread> callers;
  for (int c = 0; c < 3; ++c) {
    callers.emplace_back([&] {
      for (int round = 0; round < 20; ++round) {
        pool.ParallelFor(32, [&](std::size_t) { conc.Enter(); SleepBriefly(); conc.Leave(); });
      }
    });
  }
  for (auto &t : callers) t.join();
  CHECK(conc.max <= static_cast<int>(budget));
  CHECK(pool.PeakThreads() <= budget && pool.PeakThreads() >= 1);
  CHECK(conc.max <= static_cast<int>(pool.PeakThreads()));
  pool.ResetPeak();
  CHECK(pool.PeakThreads() == 0);
  pool.SetBudget(2);
  CHECK(pool.Budget() == 2);
  pool.ParallelFor(8, [](std::size_t) { SleepBriefly(); });
  CHECK(pool.PeakThreads() <= 2);

  // Nested ParallelFor from a task does not double count.
  ThreadBudget nested(3);
  Concurrency nc;
  nested.ParallelFor(4, [&](std::size_t) {
    nested.ParallelFor(4, [&](std::size_t) { nc.Enter(); SleepBriefly(); nc.Leave(); });
  });
  CHECK(nc.max <= 3 && nested.PeakThreads() <= 3);
}

// The services default runs tasks in index order on the calling thread.
void TestServicesParallelForDefaultSerial() {
  struct Services : accelerator_codegen::aux_state::core::AuxEngineServices {
    accelerator_codegen::aux_state::core::VisibilityToken CurrentVisibility() const override { return {}; }
    accelerator_codegen::aux_state::core::AuxStateStore &StateStore() override { throw std::logic_error("unused"); }
  } services;
  const auto self = std::this_thread::get_id();
  std::vector<std::size_t> order;
  services.ParallelFor(5, [&](std::size_t i) {
    CHECK(std::this_thread::get_id() == self);
    order.push_back(i);
  });
  CHECK((order == std::vector<std::size_t>{0, 1, 2, 3, 4}));
}

void TestParallelForException() {
  ThreadBudget pool(4);
  std::atomic<int> ran{0};
  bool caught = false;
  try {
    pool.ParallelFor(100000, [&](std::size_t i) {
      ++ran;
      if (i == 5) throw std::runtime_error("boom");
    });
  } catch (const std::runtime_error &e) {
    caught = std::string(e.what()) == "boom";
  }
  CHECK(caught && ran < 100000);
  std::atomic<int> after{0};
  pool.ParallelFor(10, [&](std::size_t) { ++after; });   // pool still usable
  CHECK(after == 10);
}

void TestSpawn() {
  ThreadBudget pool(3);
  std::atomic<int> done{0};
  auto t = pool.Spawn([&] { SleepBriefly(); ++done; });
  t->Wait();
  CHECK(t->Done() && done == 1);

  auto failing = pool.Spawn([] { throw std::runtime_error("spawn"); });
  CHECK(Throws([&] { failing->Wait(); }));

  // More tasks than budget: all still run; the over-budget ones are counted.
  pool.ResetPeak();
  std::vector<std::shared_ptr<ThreadBudget::Task>> tasks;
  std::atomic<bool> go{false};
  for (int i = 0; i < 5; ++i) {
    tasks.push_back(pool.Spawn([&] { while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1)); ++done; }));
  }
  CHECK(pool.PeakThreads() == 5);
  go = true;
  for (auto &task : tasks) task->Wait();
  CHECK(done == 6);

  // A running Spawn uses a budget slot: ParallelFor gets fewer workers.
  ThreadBudget small(2);
  go = false;
  auto holder = small.Spawn([&] { while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
  Concurrency conc;
  small.ParallelFor(16, [&](std::size_t) { conc.Enter(); SleepBriefly(); conc.Leave(); });
  CHECK(conc.max == 1);
  go = true;
  holder->Wait();

  // Destruction waits for queued work.
  auto late = std::make_shared<std::atomic<bool>>(false);
  {
    ThreadBudget scoped(2);
    scoped.Spawn([late] { SleepBriefly(); *late = true; });
  }
  CHECK(*late);
}

void TestTokenSource() {
  auto tokens = std::make_shared<FakeTokens>(2);
  ThreadBudget pool(8, tokens);
  Concurrency conc;
  pool.ParallelFor(64, [&](std::size_t) { conc.Enter(); SleepBriefly(); conc.Leave(); });
  CHECK(conc.max <= 3);                 // caller + 2 tokens
  CHECK(tokens->Available() == 2);      // all returned

  auto none = std::make_shared<FakeTokens>(0);
  ThreadBudget starved(8, none);
  Concurrency c2;
  starved.ParallelFor(16, [&](std::size_t) { c2.Enter(); SleepBriefly(); c2.Leave(); });
  CHECK(c2.max == 1);

  auto t = pool.Spawn([] {});
  t->Wait();
  CHECK(tokens->Available() == 2);
}

void TestResolveThreadBudget() {
  unsetenv("ACCEL_AUX_THREADS");
  CHECK(AffinityCpuCount() >= 1);
  CHECK(ResolveThreadBudget(7) == 7);
  CHECK(ResolveThreadBudget(0) == AffinityCpuCount());
  setenv("ACCEL_AUX_THREADS", "3", 1);
  CHECK(ResolveThreadBudget(7) == 3 && ResolveThreadBudget(0) == 3);
  setenv("ACCEL_AUX_THREADS", "0", 1);
  CHECK(ResolveThreadBudget(7) == 7);
  unsetenv("ACCEL_AUX_THREADS");

  unsetenv("ACCEL_AUX_READ_THREADS");
  unsetenv("ACCEL_AUX_BUILD_THREADS");
  CHECK(ResolveReadThreads(0, 8) == 8 && ResolveBuildThreads(0, 8) == 8);
  CHECK(ResolveReadThreads(3, 8) == 3 && ResolveReadThreads(30, 8) == 8);
  setenv("ACCEL_AUX_READ_THREADS", "2", 1);
  setenv("ACCEL_AUX_BUILD_THREADS", "5", 1);
  CHECK(ResolveReadThreads(3, 8) == 2 && ResolveBuildThreads(3, 8) == 5);
  CHECK(ResolveBuildThreads(0, 4) == 4);
  unsetenv("ACCEL_AUX_READ_THREADS");
  unsetenv("ACCEL_AUX_BUILD_THREADS");
  CHECK(ResolveMaintenanceThreads(0, 8) == 1 && ResolveMaintenanceThreads(4, 8) == 4);
  setenv("ACCEL_AUX_MAINT_THREADS", "16", 1);
  CHECK(ResolveMaintenanceThreads(0, 8) == 8);
  unsetenv("ACCEL_AUX_MAINT_THREADS");

  ThreadBudget pool(6);
  CHECK(pool.ReadLimit() == 6 && pool.BuildLimit() == 6 && pool.MaintenanceLimit() == 1);
  pool.SetPhaseLimits(2, 100, 3);
  CHECK(pool.ReadLimit() == 2 && pool.BuildLimit() == 6 && pool.MaintenanceLimit() == 3);
}

} // namespace

int main() {
  const std::pair<const char *, void (*)()> tests[] = {
      {"batch round trip", TestBatchRoundTrip},
      {"batch types and cells", TestBatchTypesAndCells},
      {"batch projection", TestBatchProjection},
      {"batch from-mutation errors", TestBatchFromMutationErrors},
      {"batch large", TestBatchLarge},
      {"applier ordering", TestApplierOrdering},
      {"applier duplicates and gaps", TestApplierDuplicatesAndGaps},
      {"applier empty commits", TestApplierEmptyCommits},
      {"applier build dense epochs", TestApplierBuildDenseEpochs},
      {"applier build sparse epochs", TestApplierBuildSparseEpochs},
      {"applier catch-up gap and resume", TestApplierCatchUpGapResume},
      {"applier empty commits in build", TestApplierEmptyCommitsInBuild},
      {"applier modes", TestApplierModes},
      {"parallel_for coverage", TestParallelForCoverage},
      {"concurrent callers", TestConcurrentCallersStayInBudget},
      {"services parallel_for default", TestServicesParallelForDefaultSerial},
      {"parallel_for exception", TestParallelForException},
      {"spawn", TestSpawn},
      {"token source", TestTokenSource},
      {"resolve thread budget", TestResolveThreadBudget},
  };
  int failed = 0;
  for (const auto &[name, fn] : tests) {
    try {
      fn();
      std::cout << "PASS " << name << "\n";
    } catch (const std::exception &e) {
      std::cout << "FAIL " << name << ": " << e.what() << "\n";
      ++failed;
    }
  }
  return failed == 0 ? 0 : 1;
}
