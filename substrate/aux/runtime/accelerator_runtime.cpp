// SCAFFOLDING
#include "substrate/aux/runtime/accelerator_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "substrate/aux/runtime/thread_budget.hpp"

namespace accelerator_codegen::aux_state::runtime {
namespace {

using Clock = std::chrono::steady_clock;
constexpr Epoch kNever = std::numeric_limits<Epoch>::max();
constexpr uint64_t kRegistrationWaitUs = 60'000'000;

struct CollectingBuilder final : core::StateBuilder {
  void Write(core::StateImage image) override { images.push_back(std::move(image)); }
  void Finish() override { finished = true; }
  std::vector<core::StateImage> images;
  bool finished = false;
};

core::InvalidationPlan TouchedPlan(const core::StagedMaintenanceOutcome &outcome) {
  auto plan = outcome.invalidations;
  MergeInvalidationPlan(plan, outcome.pending_rebuild);
  MergeInvalidationPlan(plan, core::PartitionSetPlanFromImages(outcome.updates));
  return plan;
}

// Whether publishing `outcome` leaves partitions invalidated and not rebuilt.
bool LeavesInvalidPartitions(const core::StagedMaintenanceOutcome &outcome) {
  auto plan = outcome.invalidations;
  MergeInvalidationPlan(plan, outcome.pending_rebuild);
  if (plan.IsEmpty()) {
    return false;
  }
  if (plan.scope == InvalidationScope::AllPartitions) {
    return outcome.updates.empty();
  }
  std::set<PartitionKey> updated;
  for (const auto &image : outcome.updates) {
    updated.insert(static_cast<PartitionKey>(image.partition));
  }
  for (const auto partition : plan.partitions) {
    if (!updated.count(partition)) {
      return true;
    }
  }
  return false;
}

double MicrosSince(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// The reader epoch modules see through CurrentVisibility() on this thread.
thread_local const AcceleratorRuntime *t_reader = nullptr;
thread_local Epoch t_reader_epoch = 0;

struct ReaderEpochScope {
  ReaderEpochScope(const AcceleratorRuntime *runtime, Epoch epoch)
      : saved_reader(t_reader), saved_epoch(t_reader_epoch) {
    t_reader = runtime;
    t_reader_epoch = epoch;
  }
  ~ReaderEpochScope() {
    t_reader = saved_reader;
    t_reader_epoch = saved_epoch;
  }
  const AcceleratorRuntime *saved_reader;
  Epoch saved_epoch;
};

} // namespace

struct AcceleratorRuntime::Impl {
  BuildScheduler schedule;
  std::unordered_map<core::TableId, std::vector<int32_t>> projections;

  // Writer side. Held from StageCommit to PublishCommit/AbortCommit, and
  // around every store mutation (applies, installs).
  std::mutex commit_mutex;
  struct Staged {
    Seq seq = 0;
    std::vector<ColumnarBatchRef> batches;
    std::optional<core::StagedMaintenanceOutcome> outcome; // set when synced
    bool began = false;
  };
  std::optional<Staged> staged;
  Seq next_seq = 0;
  Epoch store_epoch = 0; // what the store's epoch provider returns
  bool registration_checked = false;

  // Shared with readers.
  mutable std::mutex state_mutex;
  std::condition_variable state_cv;
  SequencedApplier applier;
  // Readers at epochs >= unserved_from cannot be served until the state is
  // synced past them. Each [from, to) in `collapsed` lost its intermediate
  // versions to a combined catch-up.
  Epoch unserved_from = 0;
  std::vector<std::pair<Epoch, Epoch>> collapsed;
  bool build_scheduled = false;
  std::optional<Clock::time_point> gap_since;
  AcceleratorStats stats;
};

AcceleratorRuntime::AcceleratorRuntime(std::shared_ptr<core::AuxQueryModule> module_p,
                                       EngineSlots &slots_p, ThreadBudget &threads_p,
                                       BuildScheduler schedule)
    : module(std::move(module_p)), id(module->Id()), slots(slots_p), threads(threads_p),
      impl(std::make_unique<Impl>()) {
  impl->schedule = schedule ? std::move(schedule)
                            : BuildScheduler([this](std::function<void()> work) {
                                threads.Spawn(std::move(work));
                              });
  for (const auto &dependency : module->Dependencies()) {
    impl->projections[dependency.table] = dependency.mutation_attnums;
  }
  VersionedAuxStore::Options options;
  options.epoch = [this] { return impl->store_epoch; };
  options.floor = [this] {
    const Epoch floor = slots.OldestReaderEpoch();
    return floor == EngineSlots::kNoFloor ? kNever : floor;
  };
  options.fixed_accelerator = id;
  store = std::make_unique<VersionedAuxStore>(std::move(options));
}

AcceleratorRuntime::~AcceleratorRuntime() = default;

std::vector<core::TableId> AcceleratorRuntime::DependencyTables() const {
  std::vector<core::TableId> tables;
  for (const auto &entry : impl->projections) {
    tables.push_back(entry.first);
  }
  return tables;
}

// ---- reads ----

bool AcceleratorRuntime::Servable(Epoch epoch, const std::optional<Epoch> &applied_through) const {
  if (epoch >= impl->unserved_from) {
    return false;
  }
  for (const auto &[from, to] : impl->collapsed) {
    if (epoch >= from && epoch < to) {
      return false;
    }
  }
  return !applied_through || impl->applier.AppliedEpoch() >= *applied_through;
}

ReadResult AcceleratorRuntime::Read(const ReadOptions &options,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  const Epoch epoch =
      options.epoch == VersionedAuxStore::kNewestSnapshot ? kNever - 1 : options.epoch;
  {
    std::unique_lock<std::mutex> lock(impl->state_mutex);
    if (!Servable(epoch, options.require_applied_through)) {
      lock.unlock();
      RequestBuild();
      lock.lock();
      impl->stats.reads_waited++;
      const auto deadline = Clock::now() + std::chrono::microseconds(options.max_wait_us);
      if (!impl->state_cv.wait_until(lock, deadline, [&] {
            return Servable(epoch, options.require_applied_through);
          })) {
        impl->stats.reads_fallback++;
        return ReadResult::Fallback;
      }
    }
  }
  ReaderEpochScope reader(this, options.epoch);
  AuxReadScope one_version;
  if (!module->CheckValidity(*this, arguments)) {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->stats.reads_fallback++;
    return ReadResult::Fallback;
  }
  module->Execute(*this, arguments, sink);
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  impl->stats.reads_hit++;
  return ReadResult::Served;
}

// ---- maintenance ----

core::StagedMaintenanceOutcome
AcceleratorRuntime::Stage(const std::vector<ColumnarBatchRef> &batches) {
  auto buffer = module->CreateTxnBuffer();
  core::InvalidationPlan plan;
  for (const auto &batch : batches) {
    const auto projection = impl->projections.find(batch->table);
    if (projection == impl->projections.end()) {
      continue;
    }
    const auto rows = ToMutationBatch(*batch, projection->second);
    MergeInvalidationPlan(plan, module->InterpretMutation(rows));
    module->AccumulateMutation(rows, *this, *buffer);
  }
  // A module that neither buffered nor invalidated could not interpret the
  // change: never publish a stale state as valid.
  if (plan.IsEmpty() && !buffer->HasPendingChanges()) {
    plan = core::AllPartitionsPlan();
  }
  return module->StageMaintenance(*this, plan, *buffer);
}

// Publishes `outcome` at `epoch`. Returns false if the state is left invalid
// (the module asked for a rebuild it did not perform): the caller rebuilds in
// the background.
bool AcceleratorRuntime::Publish(const core::StagedMaintenanceOutcome &outcome, Epoch epoch) {
  auto invalidate = outcome.invalidations;
  MergeInvalidationPlan(invalidate, outcome.pending_rebuild);
  if (outcome.action == core::MaintenanceAction::RebuildAll && invalidate.IsEmpty() &&
      !outcome.updates.empty()) {
    invalidate = core::AllPartitionsPlan();
  }
  if (outcome.action == core::MaintenanceAction::RebuildPartition &&
      invalidate.scope == InvalidationScope::AllPartitions) {
    invalidate = core::PartitionSetPlanFromImages(outcome.updates);
  }
  impl->store_epoch = epoch;
  store->FinishCommit(id, invalidate, outcome.updates, outcome.expected_version);
  return !LeavesInvalidPartitions(outcome);
}

void AcceleratorRuntime::MarkStale(Epoch from) {
  impl->applier.Invalidate();
  impl->unserved_from = std::min(impl->unserved_from, from);
}

// ---- in-process commits ----

Seq AcceleratorRuntime::StageCommit(uint64_t txn, const std::vector<ColumnarBatchRef> &batches) {
  impl->commit_mutex.lock(); // released by PublishCommit / AbortCommit
  Impl::Staged staged{++impl->next_seq, batches, std::nullopt, false};
  const Epoch visible_from = slots.PendingEpochLowerBound(txn);
  bool synced;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->applier.Announce(staged.seq);
    synced = impl->applier.CurrentMode() == SequencedApplier::Mode::Synced;
    if (!synced) {
      impl->unserved_from = std::min(impl->unserved_from, visible_from);
    }
  }
  if (synced) {
    try {
      staged.outcome = Stage(batches);
    } catch (...) {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      MarkStale(visible_from);
    }
  }
  if (staged.outcome) {
    const auto plan = TouchedPlan(*staged.outcome);
    if (!plan.IsEmpty()) {
      impl->store_epoch = visible_from;
      store->BeginCommit(id, plan);
      staged.began = true;
    }
  }
  const Seq seq = staged.seq;
  impl->staged = std::move(staged);
  return seq;
}

void AcceleratorRuntime::PublishCommit(Epoch epoch) {
  std::unique_lock<std::mutex> commit(impl->commit_mutex, std::adopt_lock);
  auto staged = std::move(*impl->staged);
  impl->staged.reset();
  const auto start = Clock::now();
  bool valid = true;
  if (staged.outcome) {
    try {
      valid = Publish(*staged.outcome, epoch);
    } catch (...) {
      store->AbortCommit(id);
      valid = false;
    }
  }
  bool stale;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (staged.outcome && valid) {
      impl->applier.MarkApplied(CommittedChange{staged.seq, epoch, {}, false});
      impl->stats.commits_applied++;
      for (const auto &batch : staged.batches) {
        impl->stats.rows_applied += batch->row_count;
      }
      impl->stats.apply_us += MicrosSince(start);
    } else {
      if (staged.outcome) {
        MarkStale(epoch); // published, but left invalid: rebuild from here
      }
      impl->applier.Offer(CommittedChange{staged.seq, epoch, std::move(staged.batches), false});
    }
    stale = impl->applier.CurrentMode() != SequencedApplier::Mode::Synced;
  }
  commit.unlock();
  impl->state_cv.notify_all();
  if (stale) {
    RequestBuild();
  }
}

void AcceleratorRuntime::AbortCommit() {
  std::unique_lock<std::mutex> commit(impl->commit_mutex, std::adopt_lock);
  auto staged = std::move(*impl->staged);
  impl->staged.reset();
  if (staged.began) {
    store->AbortCommit(id);
  }
  CommittedChange skipped{staged.seq, 0, {}, true};
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  if (staged.outcome) {
    impl->applier.MarkApplied(skipped);
  } else {
    impl->applier.Offer(std::move(skipped));
  }
}

// ---- shipped commits ----

void AcceleratorRuntime::OfferCommit(CommittedChange change) {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  impl->applier.Offer(std::move(change));
}

void AcceleratorRuntime::AnnounceCommit(Seq seq) {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  impl->applier.Announce(seq);
}

void AcceleratorRuntime::ApplyShipped(const CommittedChange &change) {
  bool valid = true;
  const auto start = Clock::now();
  if (!change.empty_commit) {
    try {
      valid = Publish(Stage(change.batches), change.epoch);
    } catch (...) {
      valid = false;
    }
  }
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  if (!valid) {
    MarkStale(change.epoch);
    impl->applier.Offer(change); // replayed by the catch-up after the rebuild
    return;
  }
  impl->applier.MarkApplied(change);
  if (!change.empty_commit) {
    impl->stats.commits_applied++;
    for (const auto &batch : change.batches) {
      impl->stats.rows_applied += batch->row_count;
    }
    impl->stats.apply_us += MicrosSince(start);
  }
}

void AcceleratorRuntime::Pump(uint64_t gap_timeout_us) {
  {
    std::lock_guard<std::mutex> commit(impl->commit_mutex);
    for (;;) {
      std::vector<CommittedChange> ready;
      {
        std::lock_guard<std::mutex> lock(impl->state_mutex);
        if (impl->applier.CurrentMode() == SequencedApplier::Mode::Synced) {
          ready = impl->applier.TakeReady();
        }
      }
      if (ready.empty()) {
        break;
      }
      for (const auto &change : ready) {
        ApplyShipped(change);
      }
    }
  }
  impl->state_cv.notify_all();
  bool rebuild;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    const auto mode = impl->applier.CurrentMode();
    if (mode == SequencedApplier::Mode::Synced && impl->applier.HasGap()) {
      if (!impl->gap_since) {
        impl->gap_since = Clock::now();
      }
      rebuild = MicrosSince(*impl->gap_since) >= gap_timeout_us;
      if (rebuild) {
        impl->stats.gaps++;
        MarkStale(impl->applier.AppliedEpoch() + 1);
      }
    } else {
      impl->gap_since.reset();
      rebuild = mode == SequencedApplier::Mode::Stale;
    }
  }
  if (rebuild) {
    RequestBuild();
  }
}

// ---- builds ----

bool AcceleratorRuntime::IsSynced() const {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  return impl->applier.CurrentMode() == SequencedApplier::Mode::Synced &&
         impl->unserved_from == kNever;
}

void AcceleratorRuntime::RequestBuild() {
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->build_scheduled || impl->applier.CurrentMode() == SequencedApplier::Mode::Synced) {
      return;
    }
    impl->build_scheduled = true;
  }
  impl->schedule([this] {
    try {
      BuildNow();
    } catch (...) {
      // Left stale; the next commit or read requests another build.
    }
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->build_scheduled = false;
  });
}

bool AcceleratorRuntime::BuildNow() {
  const auto start = Clock::now();
  {
    // No in-process commit may be staged across the switch: every commit
    // from here on is queued for the catch-up.
    std::lock_guard<std::mutex> commit(impl->commit_mutex);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->applier.CurrentMode() == SequencedApplier::Mode::Synced) {
      return true;
    }
    impl->applier.StartBuild();
  }
  try {
    if (!impl->registration_checked) {
      if (!slots.WaitForPreRegistrationWriters(DependencyTables(), kRegistrationWaitUs)) {
        throw std::runtime_error("writers from before registration did not finish: " + id);
      }
      impl->registration_checked = true;
    }
    CollectingBuilder builder;
    Epoch b;
    {
      auto snapshot = slots.OpenBuildSnapshot(*this);
      b = snapshot->SnapshotEpoch();
      {
        std::lock_guard<std::mutex> lock(impl->state_mutex);
        impl->applier.SetBuildEpoch(b);
      }
      slots.RunBuild(*this, *snapshot, builder);
    }
    if (!builder.finished) {
      throw std::logic_error("auxiliary build did not call Finish(): " + id);
    }

    std::lock_guard<std::mutex> commit(impl->commit_mutex);
    store->Install(std::move(builder.images), id, b);
    std::vector<CommittedChange> catch_up;
    {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      catch_up = impl->applier.TakeCatchUp(b);
    }
    bool valid = true;
    if (!catch_up.empty()) {
      std::vector<ColumnarBatchRef> batches;
      for (const auto &change : catch_up) {
        batches.insert(batches.end(), change.batches.begin(), change.batches.end());
      }
      valid = Publish(Stage(batches), catch_up.back().epoch);
    }
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->applier.FinishCatchUp();
    if (!valid) {
      MarkStale(catch_up.back().epoch);
    } else {
      // Readers between the first and last caught-up commit would need
      // versions the combined catch-up never produced.
      // Without a floor the store keeps only the newest version, so no reader
      // can resolve a collapsed range.
      const Epoch floor = slots.OldestReaderEpoch();
      auto &collapsed = impl->collapsed;
      collapsed.erase(std::remove_if(collapsed.begin(), collapsed.end(),
                                     [&](const auto &range) {
                                       return floor == EngineSlots::kNoFloor ||
                                              range.second <= floor;
                                     }),
                      collapsed.end());
      if (catch_up.size() > 1 && floor != EngineSlots::kNoFloor) {
        collapsed.emplace_back(catch_up.front().epoch, catch_up.back().epoch);
      }
      impl->unserved_from =
          impl->applier.HasGap() ? impl->applier.AppliedEpoch() + 1 : kNever;
    }
    impl->stats.builds++;
    impl->stats.build_us += MicrosSince(start);
    impl->stats.catch_up_commits += catch_up.size();
  } catch (...) {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->applier.AbandonBuild();
    throw;
  }
  impl->state_cv.notify_all();
  Pump(); // shipped changes that arrived after the catch-up was taken
  return IsSynced();
}

// ---- services ----

core::VisibilityToken AcceleratorRuntime::CurrentVisibility() const {
  core::VisibilityToken token;
  token.mutation_epoch = t_reader == this ? t_reader_epoch : VersionedAuxStore::kNewestSnapshot;
  return token;
}

core::AuxStateStore &AcceleratorRuntime::StateStore() { return *store; }

const maintenance::CostModelParams &AcceleratorRuntime::MaintenanceCostModel() const {
  return slots.CostModel(id);
}

double AcceleratorRuntime::ExpectedQueriesBeforeNextWrite() const {
  return slots.ExpectedQueriesBeforeNextWrite();
}

std::size_t AcceleratorRuntime::WorkerCount() const { return threads.Budget(); }

AcceleratorStats AcceleratorRuntime::Stats() const {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  return impl->stats;
}

} // namespace accelerator_codegen::aux_state::runtime
