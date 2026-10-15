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
  return std::any_of(plan.partitions.begin(), plan.partitions.end(),
                     [&](PartitionKey partition) { return !updated.count(partition); });
}

double MicrosSince(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// The reader epoch modules see through CurrentVisibility() on this thread.
thread_local const AcceleratorRuntime *t_reader = nullptr;
thread_local Epoch t_reader_epoch = 0;

} // namespace

struct AcceleratorRuntime::ReadGuard::Scope {
  Scope(const AcceleratorRuntime *runtime, Epoch epoch)
      : saved_reader(t_reader), saved_epoch(t_reader_epoch) {
    t_reader = runtime;
    t_reader_epoch = epoch;
  }
  ~Scope() {
    t_reader = saved_reader;
    t_reader_epoch = saved_epoch;
  }
  const AcceleratorRuntime *saved_reader;
  Epoch saved_epoch;
  AuxReadScope one_version;
};

AcceleratorRuntime::ReadGuard::ReadGuard(bool servable_p, std::unique_ptr<Scope> scope_p)
    : servable(servable_p), scope(std::move(scope_p)) {}
AcceleratorRuntime::ReadGuard::ReadGuard(ReadGuard &&) noexcept = default;
AcceleratorRuntime::ReadGuard::~ReadGuard() = default;

struct AcceleratorRuntime::Impl {
  BuildScheduler schedule;
  core::AuxEngineServices *module_services = nullptr;
  std::unordered_map<core::TableId, std::vector<int32_t>> projections;
  StoreHandle store;

  // Writer side. Held from BeginCommit to PublishCommit/AbortCommit, and
  // around every store mutation (applies, installs).
  std::mutex commit_mutex;
  std::mutex build_mutex; // one build at a time
  CommittedChange in_flight; // between BeginCommit and Publish/Abort
  Seq next_seq = 0;
  Epoch store_epoch = 0; // what the store's epoch provider returns
  bool registration_checked = false;

  // Shared with readers.
  mutable std::mutex state_mutex;
  std::condition_variable state_cv;
  SequencedApplier applier;
  // Readers at epochs >= unserved_from wait until the state is synced past
  // them, and those >= in_flight_from until the in-flight commit is
  // published. Each [from, to) in `collapsed` lost its intermediate versions
  // to a combined catch-up.
  Epoch unserved_from = 0;
  Epoch in_flight_from = kNever;
  std::vector<std::pair<Epoch, Epoch>> collapsed;
  bool build_scheduled = false;
  std::optional<Clock::time_point> gap_since;
  AcceleratorStats stats;

  bool Synced() const { return applier.CurrentMode() == SequencedApplier::Mode::Synced; }
};

AcceleratorRuntime::AcceleratorRuntime(std::shared_ptr<core::AuxQueryModule> module_p,
                                       EngineSlots &slots_p, ThreadBudget &threads_p,
                                       BuildScheduler schedule, StoreFactory make_store)
    : module(std::move(module_p)), id(module->Id()), slots(slots_p), threads(threads_p),
      impl(std::make_unique<Impl>()) {
  impl->module_services = this;
  impl->schedule = schedule ? std::move(schedule)
                            : BuildScheduler([this](std::function<void()> work) {
                                threads.Spawn(std::move(work));
                              });
  for (const auto &dependency : module->Dependencies()) {
    impl->projections[dependency.table] = dependency.mutation_attnums;
  }
  std::function<Epoch()> epoch = [this] { return impl->store_epoch; };
  std::function<Epoch()> floor = [this] {
    const Epoch oldest = slots.OldestReaderEpoch();
    return oldest == EngineSlots::kNoFloor ? kNever : oldest;
  };
  if (make_store) {
    impl->store = make_store(id, epoch, floor);
  }
  if (!impl->store.store) {
    VersionedAuxStore::Options options;
    options.epoch = std::move(epoch);
    options.floor = std::move(floor);
    options.fixed_accelerator = id;
    auto store = std::make_unique<VersionedAuxStore>(std::move(options));
    auto *versioned = store.get();
    impl->store.install = [versioned, this](std::vector<core::StateImage> images, Epoch at) {
      versioned->Install(std::move(images), id, at);
    };
    impl->store.store = std::move(store);
  }
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

// Transient: an in-flight commit, a stale state being rebuilt, or a shipped
// commit not applied yet. Waiting can help.
bool AcceleratorRuntime::Servable(Epoch epoch, const std::optional<Epoch> &applied_through) const {
  if (epoch >= impl->unserved_from || epoch >= impl->in_flight_from) {
    return false;
  }
  return !applied_through || impl->applier.AppliedEpoch() >= *applied_through;
}

// Permanent: the epoch's versions were collapsed by a combined catch-up.
bool AcceleratorRuntime::Collapsed(Epoch epoch) const {
  for (const auto &[from, to] : impl->collapsed) {
    if (epoch >= from && epoch < to) {
      return true;
    }
  }
  return false;
}

AcceleratorRuntime::ReadGuard AcceleratorRuntime::BeginRead(const ReadOptions &options) {
  const Epoch epoch =
      options.epoch == VersionedAuxStore::kNewestSnapshot ? kNever - 1 : options.epoch;
  std::unique_lock<std::mutex> lock(impl->state_mutex);
  if (Collapsed(epoch)) {
    impl->stats.reads_fallback++;
    return ReadGuard(false, nullptr);
  }
  if (!Servable(epoch, options.require_applied_through)) {
    lock.unlock();
    RequestBuild();
    lock.lock();
    impl->stats.reads_waited++;
    const auto deadline = Clock::now() + std::chrono::microseconds(options.max_wait_us);
    if (!impl->state_cv.wait_until(lock, deadline, [&] {
          return Servable(epoch, options.require_applied_through) || Collapsed(epoch);
        }) ||
        Collapsed(epoch)) {
      impl->stats.reads_fallback++;
      return ReadGuard(false, nullptr);
    }
  }
  impl->stats.reads_hit++;
  return ReadGuard(true, std::make_unique<ReadGuard::Scope>(this, options.epoch));
}

ReadResult AcceleratorRuntime::Read(const ReadOptions &options,
                                    const core::QueryArguments &arguments,
                                    core::AuxRowSink &sink) {
  auto guard = BeginRead(options);
  if (!guard.Servable() || !module->CheckValidity(*this, arguments)) {
    return ReadResult::Fallback;
  }
  module->Execute(*this, arguments, sink);
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
    module->AccumulateMutation(rows, *impl->module_services, *buffer);
  }
  // A module that neither buffered nor invalidated could not interpret the
  // change: never publish a stale state as valid.
  if (plan.IsEmpty() && !buffer->HasPendingChanges()) {
    plan = core::AllPartitionsPlan();
  }
  return module->StageMaintenance(*impl->module_services, plan, *buffer);
}

// Publishes `outcome` at `epoch`. Returns false if the state is left invalid
// (the module asked for a rebuild it did not perform).
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
  auto &store = *impl->store.store;
  impl->store_epoch = epoch;
  store.BeginCommit(id, invalidate);
  store.FinishCommit(id, invalidate, outcome.updates, outcome.expected_version);
  return !LeavesInvalidPartitions(outcome);
}

// Maintains and publishes one committed change. False: the state is now
// invalid and the change must be replayed after a rebuild.
bool AcceleratorRuntime::ApplyChange(const CommittedChange &change) {
  if (change.invalidates) {
    return false;
  }
  if (change.empty_commit) {
    return true;
  }
  core::ClearMaintenanceDecisionLog();
  try {
    const bool valid = Publish(Stage(change.batches), change.epoch);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->stats.last_decision = core::LastMaintenanceDecision();
    return valid;
  } catch (...) {
    return false;
  }
}

// Bookkeeping for an applied change (state_mutex held).
void AcceleratorRuntime::Applied(const CommittedChange &change, double us) {
  impl->applier.MarkApplied(change);
  if (change.empty_commit) {
    return;
  }
  impl->stats.commits_applied++;
  for (const auto &batch : change.batches) {
    impl->stats.rows_applied += batch->row_count;
  }
  impl->stats.apply_us += us;
}

void AcceleratorRuntime::MarkStale(Epoch from) {
  impl->applier.Invalidate();
  impl->unserved_from = std::min(impl->unserved_from, from);
}

// ---- in-process commits ----

Seq AcceleratorRuntime::BeginCommit(uint64_t txn, std::vector<ColumnarBatchRef> batches) {
  impl->commit_mutex.lock(); // released by PublishCommit / AbortCommit
  impl->in_flight = CommittedChange{++impl->next_seq, 0, std::move(batches), false};
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  impl->applier.Announce(impl->in_flight.seq);
  impl->in_flight_from = slots.PendingEpochLowerBound(txn);
  return impl->in_flight.seq;
}

void AcceleratorRuntime::PublishCommit(Epoch epoch) {
  std::unique_lock<std::mutex> commit(impl->commit_mutex, std::adopt_lock);
  auto change = std::move(impl->in_flight);
  change.epoch = epoch;
  bool synced;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    synced = impl->Synced();
  }
  const auto start = Clock::now();
  const bool applied = synced && ApplyChange(change);
  bool stale;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->in_flight_from = kNever;
    if (applied) {
      Applied(change, MicrosSince(start));
    } else {
      if (synced) {
        MarkStale(epoch);
      }
      impl->applier.Offer(std::move(change)); // replayed by the catch-up
    }
    stale = !impl->Synced();
  }
  commit.unlock();
  impl->state_cv.notify_all();
  if (stale) {
    RequestBuild();
  }
}

void AcceleratorRuntime::AbortCommit() {
  std::unique_lock<std::mutex> commit(impl->commit_mutex, std::adopt_lock);
  CommittedChange skipped{impl->in_flight.seq, 0, {}, true};
  impl->in_flight = CommittedChange{};
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    impl->in_flight_from = kNever;
    if (impl->Synced()) {
      Applied(skipped, 0);
    } else {
      impl->applier.Offer(std::move(skipped));
    }
  }
  commit.unlock();
  impl->state_cv.notify_all();
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

// Applies the ready changes (commit_mutex held). Everything the engine's
// maintenance lookups already see is applied as ONE combined change, so the
// lookups match the state it produces; the versions in between are never
// published (readers there fall back).
void AcceleratorRuntime::ApplyReady() {
  for (;;) {
    std::vector<CommittedChange> ready;
    {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      if (impl->Synced()) {
        ready = impl->applier.TakeReady();
      }
    }
    if (ready.empty()) {
      return;
    }
    Epoch last = 0;
    for (const auto &change : ready) {
      last = std::max(last, change.epoch);
    }
    const Epoch seen = slots.MaintenanceLookupEpoch(last);
    auto later = std::stable_partition(ready.begin(), ready.end(),
                                       [&](const auto &change) { return change.epoch <= seen; });
    std::vector<CommittedChange> held(std::make_move_iterator(later),
                                      std::make_move_iterator(ready.end()));
    ready.erase(later, ready.end());
    auto requeue = [&](std::vector<CommittedChange> &changes) {
      for (auto &change : changes) {
        impl->applier.Offer(std::move(change));
      }
    };
    // seen > last: the lookups see commits that have not arrived yet; wait.
    if (ready.empty() || seen > last) {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      requeue(ready);
      requeue(held);
      return;
    }
    CommittedChange combined{ready.back().seq, 0, {}, true};
    Epoch first = 0;
    for (const auto &change : ready) {
      combined.epoch = std::max(combined.epoch, change.epoch);
      combined.invalidates = combined.invalidates || change.invalidates;
      if (!change.empty_commit) {
        combined.empty_commit = false;
        first = first ? first : change.epoch;
        combined.batches.insert(combined.batches.end(), change.batches.begin(),
                                change.batches.end());
      }
    }
    const auto start = Clock::now();
    const bool applied = ApplyChange(combined);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (!applied) {
      // Replayed after the rebuild.
      MarkStale(first ? first : combined.epoch);
      requeue(ready);
      requeue(held);
      return;
    }
    const double us = MicrosSince(start);
    for (const auto &change : ready) {
      Applied(change, us / ready.size());
    }
    if (first && first < combined.epoch) {
      Collapse(first, combined.epoch);
    }
    if (!held.empty()) {
      requeue(held);
      return;
    }
  }
}

// [from, to) lost its intermediate versions (state_mutex held). Without a
// floor the store keeps only the newest version, so none could resolve them
// anyway; ranges every reader is past are dropped.
void AcceleratorRuntime::Collapse(Epoch from, Epoch to) {
  const Epoch floor = slots.OldestReaderEpoch();
  auto &collapsed = impl->collapsed;
  collapsed.erase(std::remove_if(collapsed.begin(), collapsed.end(),
                                 [&](const auto &range) {
                                   return floor == EngineSlots::kNoFloor ||
                                          range.second <= floor;
                                 }),
                  collapsed.end());
  if (floor != EngineSlots::kNoFloor) {
    collapsed.emplace_back(from, to);
  }
}

void AcceleratorRuntime::Pump(uint64_t gap_timeout_us) {
  {
    std::lock_guard<std::mutex> commit(impl->commit_mutex);
    ApplyReady();
  }
  impl->state_cv.notify_all();
  bool rebuild;
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->Synced() && impl->applier.HasGap()) {
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
      rebuild = !impl->Synced();
    }
  }
  if (rebuild) {
    RequestBuild();
  }
}

// ---- builds ----

bool AcceleratorRuntime::IsSynced() const {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  return impl->Synced() && impl->unserved_from == kNever;
}

void AcceleratorRuntime::Invalidate() {
  std::lock_guard<std::mutex> commit(impl->commit_mutex);
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  if (impl->Synced()) {
    MarkStale(0);
  }
}

void AcceleratorRuntime::UseModuleServices(core::AuxEngineServices &services) {
  impl->module_services = &services;
}

void AcceleratorRuntime::RequestBuild() {
  {
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->build_scheduled || impl->Synced()) {
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
  // A caller arriving during a build waits for it, and then finds the state
  // synced (or builds again if it is still stale).
  std::lock_guard<std::mutex> one_build(impl->build_mutex);
  const auto start = Clock::now();
  {
    // No in-process commit may be in flight across the switch: every commit
    // from here on is queued for the catch-up.
    std::lock_guard<std::mutex> commit(impl->commit_mutex);
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->Synced()) {
      return true;
    }
    impl->applier.StartBuild();
  }
  std::vector<CommittedChange> catch_up;
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
    impl->store.install(std::move(builder.images), b);
    {
      // Synced at the build; what it lacks is applied below like any change.
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      catch_up = impl->applier.TakeCatchUp(b);
      impl->stats.catch_up_commits += catch_up.size();
      impl->applier.ReturnCatchUp(std::move(catch_up));
      impl->applier.FinishCatchUp();
    }
    ApplyReady();
    std::lock_guard<std::mutex> lock(impl->state_mutex);
    if (impl->Synced()) {
      impl->unserved_from = impl->applier.HasGap() ? impl->applier.AppliedEpoch() + 1 : kNever;
    }
    impl->stats.builds++;
    impl->stats.build_us += MicrosSince(start);
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

core::AuxStateStore &AcceleratorRuntime::StateStore() { return *impl->store.store; }

const maintenance::CostModelParams &AcceleratorRuntime::MaintenanceCostModel() const {
  return slots.CostModel(id);
}

double AcceleratorRuntime::ExpectedQueriesBeforeNextWrite() const {
  return slots.ExpectedQueriesBeforeNextWrite(id);
}

std::size_t AcceleratorRuntime::WorkerCount() const { return threads.Budget(); }

void AcceleratorRuntime::ParallelFor(std::size_t n, const std::function<void(std::size_t)> &fn) const {
  threads.ParallelFor(n, fn);
}

AcceleratorStats AcceleratorRuntime::Stats() const {
  std::lock_guard<std::mutex> lock(impl->state_mutex);
  return impl->stats;
}

} // namespace accelerator_codegen::aux_state::runtime
