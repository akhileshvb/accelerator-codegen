// GENERATED BACKEND
//
// Shard workers: lifecycle, the worker's main loop, and the writer- and
// diagnostics-facing entry points. Reads are in remote_read.cpp; see
// shard_worker.hpp for the design.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

#include "backends/postgres/state/shard_worker.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
#include "backends/postgres/api/engine_helpers.hpp"
#include "backends/postgres/engine/accelerator_tables.hpp"
#include "backends/postgres/state/shard_gucs.hpp"
#include "backends/postgres/state/shard_server.hpp"
#include "backends/postgres/state/shard_slots.hpp"
#include "backends/postgres/state/shared.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"
#include "substrate/aux/runtime/runtime_host.hpp"

extern "C" {
#include <access/xact.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <postmaster/bgworker.h>
#include <postmaster/interrupt.h>
#include <storage/dsm_registry.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/pmsignal.h>
#include <tcop/tcopprot.h>
#include <utils/guc.h>
#include <utils/timestamp.h>
}

namespace accelerator_codegen::aux_state::engine::postgres::shard {
namespace {

using std::chrono::milliseconds;

constexpr auto kStartupWait = milliseconds(20000);
constexpr auto kStopWait = milliseconds(10000);
// How long a maintenance pass waits for the commits its snapshot sees to
// reach the worker (they are shipped right after they commit).
constexpr auto kArrivalWait = milliseconds(50);
// Maintenance also runs without commits arriving: gaps and the version floor.
constexpr auto kIdlePump = milliseconds(250);
constexpr auto kHorizonEvery = milliseconds(250);

bool Alive(const ShardSegment &segment) {
  if (segment.state.load() != ShardSegment::kRunning) {
    return false;
  }
  const pid_t pid = static_cast<pid_t>(segment.pid.load());
  return pid > 0 && !(kill(pid, 0) != 0 && errno == ESRCH);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Waits (interruptibly) until `done`, at most `limit`.
template <typename Done>
bool WaitUntil(milliseconds limit, Done &&done) {
  const auto deadline = Clock::now() + limit;
  while (!done()) {
    if (Clock::now() >= deadline) {
      return false;
    }
    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10, PG_WAIT_EXTENSION);
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
  }
  return true;
}

std::string ModulePath() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(&accel_shard_worker_main), &info) && info.dli_fname &&
      *info.dli_fname) {
    return info.dli_fname;
  }
  return "accelerator_codegen_pg";
}

// Registers the worker and waits until it accepts requests.
bool Launch(ShardSegment &segment, int index) {
  segment.started_ns = NowNs();
  BackgroundWorker worker;
  std::memset(&worker, 0, sizeof(worker));
  worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
  worker.bgw_start_time = BgWorkerStart_ConsistentState;
  worker.bgw_restart_time = BGW_NEVER_RESTART;
  const std::string library = ModulePath();
  snprintf(worker.bgw_library_name, BGW_MAXLEN, "%s", library.c_str());
  snprintf(worker.bgw_function_name, BGW_MAXLEN, "accel_shard_worker_main");
  snprintf(worker.bgw_name, BGW_MAXLEN, "accel shard worker (database %u accelerator %d)",
           static_cast<unsigned>(MyDatabaseId), index);
  snprintf(worker.bgw_type, BGW_MAXLEN, "accel shard worker");
  worker.bgw_main_arg = ObjectIdGetDatum(MyDatabaseId);
  const int32 index_arg = index;
  std::memcpy(worker.bgw_extra, &index_arg, sizeof(index_arg));
  worker.bgw_notify_pid = MyProcPid;
  BackgroundWorkerHandle *handle = nullptr;
  pid_t pid = 0;
  if (!RegisterDynamicBackgroundWorker(&worker, &handle) ||
      WaitForBackgroundWorkerStartup(handle, &pid) != BGWH_STARTED) {
    return false;
  }
  return WaitUntil(kStartupWait,
                   [&] { return segment.state.load() != ShardSegment::kStarting; }) &&
         Alive(segment);
}

// Makes sure shard `index`'s worker runs, starting it if need be.
bool StartWorker(int index) {
  ShardSegment *segment = Segment(index);
  if (!segment) {
    return false;
  }
  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t state = segment->state.load();
    if (state == ShardSegment::kRunning) {
      if (Alive(*segment)) {
        return true;
      }
      segment->state.compare_exchange_strong(state, ShardSegment::kStopped); // died without its exit hook
      continue;
    }
    if (state == ShardSegment::kStarting || state == ShardSegment::kStopping) {
      // Another backend is starting it (or did and died), or the old one is leaving.
      const bool up = WaitUntil(kStartupWait, [&] { return segment->state.load() != state; });
      if (up) {
        continue;
      }
      segment->state.compare_exchange_strong(state, ShardSegment::kStopped);
      continue;
    }
    if (segment->state.compare_exchange_strong(state, ShardSegment::kStarting)) {
      if (Launch(*segment, index)) {
        return true;
      }
      state = ShardSegment::kStarting;
      segment->state.compare_exchange_strong(state, ShardSegment::kStopped);
      return false;
    }
  }
  return false;
}

// ---- the worker ----

ShardSegment *g_segment = nullptr; // the worker's own shard
ShardServer *g_server = nullptr;
runtime::RuntimeHost *g_host = nullptr;
int g_index = -1;

void WorkerExit(int, Datum) {
  if (g_segment) {
    g_segment->state = ShardSegment::kStopping;
  }
  if (g_host) { // readers wait for a commit that is applied in part: it never ends
    g_host->AbandonDeferredCommits();
    g_host->StopReads();
  }
  if (g_server) {
    g_server->Stop();
  }
  if (g_index >= 0) {
    shared::ReleaseHeldTokens(g_index);
  }
  if (g_segment) {
    g_segment->Close();
    g_segment->pid = 0;
    g_segment->state = ShardSegment::kStopped;
  }
}

// Publishes what the diagnostics read, and takes what they set.
class Diagnostics {
public:
  Diagnostics(ShardSegment &segment_p, std::vector<runtime::AcceleratorRuntime *> runtimes_p)
      : segment(segment_p), runtimes(std::move(runtimes_p)) {}

  void Publish() {
    uint64_t applied = 0, builds = 0, gaps = 0, synced = 0;
    for (auto *runtime : runtimes) {
      const auto stats = runtime->Stats();
      applied += stats.commits_applied;
      builds += stats.builds;
      gaps += stats.gaps;
      synced += runtime->IsSynced();
      if (stats.last_decision) {
        core::SetGlobalMaintenanceDecision(*stats.last_decision);
        std::string decision = core::FormatLastMaintenanceDecision();
        std::string &published = published_decision[runtime];
        if (decision != published) {
          segment.SetText(ShardSegment::kLastDecision, decision);
          segment.SetText(ShardSegment::kLastAction,
                          maintenance::MaintenanceActionName(stats.last_decision->staged_action));
          segment.stats.decision_ns = NowNs();
          published = std::move(decision);
        }
      }
    }
    segment.stats.applied = applied;
    segment.stats.builds = builds;
    segment.stats.gaps = gaps;
    segment.stats.synced = synced;
    segment.stats.members = runtimes.size();
  }

  // Takes the forced maintenance action diagnostics set (before the next apply).
  void TakeForced() {
    const std::string forced = segment.GetText(ShardSegment::kForcedAction);
    if (forced != applied_forced) {
      applied_forced = forced;
      core::ClearMaintenanceDecisionLog();
      core::SetForcedMaintenanceActionName(forced);
    }
  }

private:
  ShardSegment &segment;
  std::vector<runtime::AcceleratorRuntime *> runtimes;
  std::map<runtime::AcceleratorRuntime *, std::string> published_decision;
  std::string applied_forced;
};

// Applies the phase caps (at start and on every config reload). The build's
// scans decode in parallel: this thread plus the build cap's other threads as
// Postgres parallel workers.
void ApplyPhaseLimits(runtime::ThreadBudget &threads) {
  const std::size_t budget = threads.Budget();
  threads.SetPhaseLimits(runtime::ResolveReadThreads(PhaseReadThreads(), budget),
                         runtime::ResolveBuildThreads(PhaseBuildThreads(), budget),
                         runtime::ResolveMaintenanceThreads(PhaseMaintenanceThreads(), budget));
  SetBuildScanParticipants(static_cast<int>(threads.BuildLimit()));
  elog(LOG, "accel shard worker phase caps: read=%zu build=%zu maint=%zu", threads.ReadLimit(),
       threads.BuildLimit(), threads.MaintenanceLimit());
}

[[noreturn]] void RunWorker(int index) {
  ShardSegment *segment = Segment(index);
  if (!segment || segment->state.load() != ShardSegment::kStarting) {
    proc_exit(0); // a starter that gave up has started another worker
  }
  g_segment = segment;
  g_index = index;
  shared::ReleaseHeldTokens(index); // a predecessor that died holding some
  before_shmem_exit(WorkerExit, 0);
  segment->pid = MyProcPid;
  segment->generation++;
  segment->Reset();
  segment->SetText(ShardSegment::kSearchPath, GetConfigOption("search_path", false, false));
  segment->SetText(ShardSegment::kLastError, "");

  const core::AcceleratorId id = shared::IdOf(index);
  StartTransactionCommand();
  const auto members = ShardMembers(id);
  if (members.empty()) {
    elog(FATAL, "accelerator %s is not compiled into this module", id.c_str());
  }
  std::vector<runtime::TableMap> tables;
  for (auto *member : members) {
    tables.push_back(HostTableMap(*member));
  }
  CommitTransactionCommand();

  CommitArrivals arrivals;
  PgSlots slots(index, members, arrivals);
  runtime::RuntimeHost host(slots, shared::ThreadBudget(), shared::ThreadTokens(index));
  ApplyPhaseLimits(host.Threads());
  g_host = &host;
  std::vector<runtime::AcceleratorRuntime *> runtimes;
  for (std::size_t i = 0; i < members.size(); i++) {
    runtimes.push_back(&host.Register(members[i]->Module(), members[i]->Stores(), tables[i]));
  }
  slots.SetPerCommit(PerCommitVersions() &&
                     std::any_of(members.begin(), members.end(), [](auto *m) { return m->Stores() != nullptr; }));
  slots.NotePreRegistrationWriters();
  ShardServer server(*segment, index, runtimes, arrivals);
  g_server = &server;
  for (std::size_t i = 0; i < members.size(); i++) {
    if (members[i]->Stores()) { // a verified store: its readers wait for the state, never for SQL
      server.SetPatient(i);
    }
  }
  server.Start(ReadThreads());
  // From here on, writers ship their commits (those visible before were
  // dropped, and are in the first build: its snapshot comes after this).  A restarted worker never sees
  // the epochs before it: its passes must not wait for them.
  arrivals.Covered(shared::AssignedEpoch(index));
  segment->state = ShardSegment::kRunning;
  elog(LOG, "accel shard worker started: %s (%zu accelerators, index %d), database %u", id.c_str(),
       members.size(), index, static_cast<unsigned>(MyDatabaseId));

  Diagnostics diagnostics(*segment, runtimes);
  // A verified store: its writers wait while a build is under way and a reader waits (shared.cpp AwaitCatchUp).
  const bool exact_versions = std::any_of(members.begin(), members.end(), [](auto *m) { return m->Stores() != nullptr; });
  const auto applied_through = [&] {
    runtime::Epoch through = std::numeric_limits<runtime::Epoch>::max();
    for (auto *runtime : runtimes) {
      through = std::min(through, runtime->AppliedThrough());
    }
    return through;
  };
  if (exact_versions) {
    shared::PublishCatchingUp(index, true); // until the first pass has found every member synced
  }
  // What the maintenance snapshots may be released for: the state of every synced member is applied
  // through it (no member synced: nothing to look up).
  const auto synced_through = [&] {
    runtime::Epoch through = std::numeric_limits<runtime::Epoch>::max();
    for (auto *runtime : runtimes) {
      if (runtime->IsSynced()) {
        through = std::min(through, runtime->AppliedThrough());
      }
    }
    return through;
  };
  const auto applied_sum = [&] {
    uint64_t sum = 0;
    for (auto *runtime : runtimes) {
      sum += runtime->AppliedThrough();
    }
    return sum;
  };
  const auto all_synced = [&] {
    return std::all_of(runtimes.begin(), runtimes.end(), [](auto *runtime) { return runtime->IsSynced(); });
  };
  uint64_t commits_seen = 0;
  auto last_pump = Clock::time_point();
  auto last_horizon = Clock::time_point();
  // The members apply a shipped commit together and end it together: the world's one version per commit.
  host.GroupCommitEnds();
  for (;;) {
    CHECK_FOR_INTERRUPTS();
    if (!PostmasterIsAlive()) {
      proc_exit(1);
    }
    if (ConfigReloadPending) {
      ConfigReloadPending = false;
      ProcessConfigFile(PGC_SIGHUP);
      ApplyPhaseLimits(host.Threads());
    }
    const bool woken = server.WaitWork(milliseconds(all_synced() ? 50 : 5));
    if (server.Failed()) {
      elog(LOG, "accel shard worker %s: transport failed (%s); restarting on demand", id.c_str(),
           segment->GetText(ShardSegment::kLastError).c_str());
      proc_exit(1);
    }

    const uint64_t commits = segment->stats.commits.load();
    const auto now = Clock::now();
    if (woken || commits != commits_seen || now - last_pump >= kIdlePump) {
      commits_seen = commits;
      last_pump = now;
      for (auto *runtime : runtimes) {
        runtime->AnnounceCommit(shared::AssignedEpoch(index));
      }
      // One maintenance pass under one snapshot S (epoch s): once every commit through s has arrived,
      // each synced member applies them, one version per commit (or one at s without per-commit
      // lookups); then the builds the pass asked for read S too, so a rebuilt member joins the others at
      // s and the commits after s apply on top.
      diagnostics.TakeForced();
      const auto epoch = slots.BeginMaintenance();
      const bool arrived = !epoch || arrivals.WaitThrough(*epoch, now + kArrivalWait);
      bool again = false;
      // One commit per member and round when the lookups are per commit, all that are ready otherwise.
      for (bool progressed = arrived; progressed;) {
        const uint64_t before = applied_sum();
        for (auto *runtime : runtimes) {
          runtime->Pump();
        }
        again = host.EndDeferredCommits(); // a member waited for the commit that just ended
        progressed = applied_sum() != before;
        if (exact_versions && all_synced()) {
          shared::PublishApplied(index, applied_through());
        }
      }
      if (!arrived) {
        again = host.EndDeferredCommits();
      }
      if (arrived && epoch && !again) {
        if (exact_versions) {
          shared::PublishCatchingUp(index, !all_synced());
        }
        again = host.RunEngineWork(); // builds, on this thread
        host.EndDeferredCommits();
      }
      if (exact_versions) {
        shared::PublishCatchingUp(index, !all_synced());
        shared::PublishApplied(index, arrived && epoch && !again && applied_through() >= *epoch ? *epoch : 0);
      }
      if (again || !arrived) {
        last_pump = Clock::time_point();
        server.Wake();
      }
      if (now - last_horizon >= kHorizonEvery) {
        last_horizon = now;
        slots.RefreshHorizon();
      }
      slots.EndMaintenance(synced_through());
      diagnostics.Publish();
    }

    const int idle_exit = IdleExitSeconds();
    if (idle_exit > 0 && Clock::now() - server.LastActivity() > std::chrono::seconds(idle_exit)) {
      elog(LOG, "accel shard worker %s: idle, exiting", id.c_str());
      proc_exit(0);
    }
  }
}

// ---- diagnostics ----

// The running shard workers of this database.
std::vector<ShardSegment *> RunningSegments() {
  std::vector<ShardSegment *> running;
  for (const int index : shared::Active()) {
    if (ShardSegment *segment = Segment(index); segment && Alive(*segment)) {
      running.push_back(segment);
    }
  }
  return running;
}

} // namespace

bool WorkerRunning(int index) {
  const ShardSegment *segment = Segment(index);
  return segment && Alive(*segment);
}

ShardSegment *Segment(int index) {
  static Oid database = InvalidOid;
  static ShardSegment *cache[shared::kMaxAccelerators] = {};
  if (index < 0 || index >= shared::kMaxAccelerators) {
    return nullptr;
  }
  if (database != MyDatabaseId) { // a backend stays in one database
    std::fill(std::begin(cache), std::end(cache), nullptr);
    database = MyDatabaseId;
  }
  if (!cache[index]) {
    const std::string name = "accel_shard_" + std::to_string(static_cast<unsigned>(database)) + "_" +
                             std::to_string(index);
    bool found = false;
    cache[index] = static_cast<ShardSegment *>(GetNamedDSMSegment(
        name.c_str(), ShardSegment::Size(), [](void *memory) { ShardSegment::Create(memory); }, &found));
  }
  return cache[index];
}

void EnsureWorker(int index) { (void)StartWorker(index); }

bool SendCommit(int index, std::string_view message, Clock::time_point deadline) {
  ShardSegment *segment = Segment(index);
  if (!segment || segment->state.load() != ShardSegment::kRunning) {
    return true;
  }
  return segment->SendCommit(message, deadline);
}

std::string StatsText() {
  const auto running = RunningSegments();
  if (running.empty()) {
    return "no worker";
  }
  uint64_t requests = 0, served = 0, fallback = 0, errors = 0, commits = 0, applied = 0, builds = 0,
           gaps = 0, synced = 0, members = 0;
  uint32_t generation = 0;
  for (const ShardSegment *segment : running) {
    requests += segment->stats.requests;
    served += segment->stats.served;
    fallback += segment->stats.fallback;
    errors += segment->stats.errors;
    commits += segment->stats.commits;
    applied += segment->stats.applied;
    builds += segment->stats.builds;
    gaps += segment->stats.gaps;
    synced += segment->stats.synced;
    members += segment->stats.members;
    generation = std::max(generation, segment->generation.load());
  }
  char text[400];
  snprintf(text, sizeof(text),
           "requests=%llu served=%llu fallback=%llu errors=%llu commits=%llu applied=%llu "
           "builds=%llu gaps=%llu synced=%llu members=%llu workers=%zu generation=%u pid=%u",
           (unsigned long long)requests, (unsigned long long)served, (unsigned long long)fallback,
           (unsigned long long)errors, (unsigned long long)commits, (unsigned long long)applied,
           (unsigned long long)builds, (unsigned long long)gaps, (unsigned long long)synced,
           (unsigned long long)members, running.size(), generation, running.front()->pid.load());
  return text;
}

bool StopWorkers() {
  const auto running = RunningSegments();
  for (const ShardSegment *segment : running) {
    kill(static_cast<pid_t>(segment->pid.load()), SIGTERM);
  }
  WaitUntil(kStopWait, [&] {
    return std::none_of(running.begin(), running.end(), [](const ShardSegment *segment) {
      return segment->state.load() == ShardSegment::kRunning;
    });
  });
  return !running.empty();
}

void SetForcedMaintenance(const std::string &action_name) {
  for (const int index : shared::Active()) {
    if (ShardSegment *segment = Segment(index)) {
      segment->SetText(ShardSegment::kForcedAction, action_name);
    }
  }
}

namespace {
// The shard whose worker made a maintenance decision last.
ShardSegment *LatestDecision() {
  ShardSegment *latest = nullptr;
  for (const int index : shared::Active()) {
    ShardSegment *segment = Segment(index);
    if (segment && segment->stats.decision_ns > 0 &&
        (!latest || segment->stats.decision_ns > latest->stats.decision_ns)) {
      latest = segment;
    }
  }
  return latest;
}
} // namespace

std::string LastMaintenanceActionName() {
  ShardSegment *segment = LatestDecision();
  return segment ? segment->GetText(ShardSegment::kLastAction) : std::string();
}

std::string LastMaintenanceDecisionText() {
  ShardSegment *segment = LatestDecision();
  return segment ? segment->GetText(ShardSegment::kLastDecision) : std::string();
}

} // namespace accelerator_codegen::aux_state::engine::postgres::shard

extern "C" {

void accel_shard_worker_main(Datum main_arg) {
  const Oid database = DatumGetObjectId(main_arg);
  int32 index;
  std::memcpy(&index, MyBgworkerEntry->bgw_extra, sizeof(index));
  pqsignal(SIGTERM, die);
  pqsignal(SIGHUP, SignalHandlerForConfigReload);
  BackgroundWorkerUnblockSignals();
  BackgroundWorkerInitializeConnectionByOid(database, InvalidOid, 0);
  SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
  SetConfigOption("application_name", "accel shard worker", PGC_SUSET, PGC_S_OVERRIDE);
  accelerator_codegen::aux_state::engine::postgres::shard::RunWorker(index);
}

} // extern "C"

#endif
