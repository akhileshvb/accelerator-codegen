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
#include <memory>
#include <string>
#include <vector>

#include "backends/postgres/api/accelerators.hpp"
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

void WorkerExit(int, Datum) {
  if (g_segment) {
    g_segment->state = ShardSegment::kStopping;
  }
  if (g_server) {
    g_server->Stop();
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
  Diagnostics(ShardSegment &segment_p, runtime::AcceleratorRuntime &runtime_p)
      : segment(segment_p), runtime(runtime_p) {}

  void Publish() {
    const auto stats = runtime.Stats();
    segment.stats.applied = stats.commits_applied;
    segment.stats.builds = stats.builds;
    segment.stats.gaps = stats.gaps;
    segment.stats.synced = runtime.IsSynced();
    segment.stats.waited = stats.reads_waited;
    segment.stats.declined = stats.reads_fallback;
    if (stats.last_decision) {
      core::SetGlobalMaintenanceDecision(*stats.last_decision);
      std::string decision = core::FormatLastMaintenanceDecision();
      if (decision != published_decision) {
        segment.SetText(ShardSegment::kLastDecision, decision);
        segment.SetText(ShardSegment::kLastAction,
                        maintenance::MaintenanceActionName(stats.last_decision->staged_action));
        segment.stats.decision_ns = NowNs();
        published_decision = std::move(decision);
      }
    }
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
  runtime::AcceleratorRuntime &runtime;
  std::string published_decision;
  std::string applied_forced;
};

[[noreturn]] void RunWorker(int index) {
  ShardSegment *segment = Segment(index);
  if (!segment || segment->state.load() != ShardSegment::kStarting) {
    proc_exit(0); // a starter that gave up has started another worker
  }
  g_segment = segment;
  before_shmem_exit(WorkerExit, 0);
  segment->pid = MyProcPid;
  segment->generation++;
  segment->Reset();
  segment->SetText(ShardSegment::kSearchPath, GetConfigOption("search_path", false, false));
  segment->SetText(ShardSegment::kLastError, "");

  const core::AcceleratorId id = shared::IdOf(index);
  StartTransactionCommand();
  PostgresAccelerator *accelerator = FindPostgresAccelerator(id);
  if (!accelerator) {
    elog(FATAL, "accelerator %s is not compiled into this module", id.c_str());
  }
  const runtime::TableMap tables = HostTableMap(*accelerator);
  CommitTransactionCommand();

  CommitArrivals arrivals;
  PgSlots slots(index, *accelerator, arrivals);
  runtime::RuntimeHost host(slots, shared::ThreadBudget(), shared::ThreadTokens());
  runtime::AcceleratorRuntime &runtime = host.Register(accelerator->Module(), nullptr, tables);
  slots.NotePreRegistrationWriters();
  ShardServer server(*segment, index, runtime, arrivals);
  g_server = &server;
  server.Start(ReadThreads());
  // From here on, writers ship their commits (those visible before were
  // dropped, and are in the first build: its snapshot comes after this).
  segment->state = ShardSegment::kRunning;
  elog(LOG, "accel shard worker started: accelerator %s (index %d), database %u", id.c_str(), index,
       static_cast<unsigned>(MyDatabaseId));

  Diagnostics diagnostics(*segment, runtime);
  uint64_t commits_seen = 0;
  auto last_pump = Clock::time_point();
  auto last_horizon = Clock::time_point();
  for (;;) {
    CHECK_FOR_INTERRUPTS();
    if (!PostmasterIsAlive()) {
      proc_exit(1);
    }
    if (ConfigReloadPending) {
      ConfigReloadPending = false;
      ProcessConfigFile(PGC_SIGHUP);
    }
    server.WaitWork(milliseconds(runtime.IsSynced() ? 50 : 5));
    if (server.Failed()) {
      elog(LOG, "accel shard worker %s: transport failed (%s); restarting on demand", id.c_str(),
           segment->GetText(ShardSegment::kLastError).c_str());
      proc_exit(1);
    }
    const bool built = host.RunEngineWork(); // builds, on this thread
    slots.EndMaintenance();                  // the catch-up of a build

    const uint64_t commits = segment->stats.commits.load();
    const auto now = Clock::now();
    if (built || commits != commits_seen || now - last_pump >= kIdlePump) {
      commits_seen = commits;
      last_pump = now;
      runtime.AnnounceCommit(shared::AssignedEpoch(index));
      // Lookups see what the snapshot sees: apply once every commit up to its
      // epoch has arrived.
      diagnostics.TakeForced();
      const auto epoch = slots.BeginMaintenance();
      if (epoch && runtime.IsSynced()) {
        arrivals.WaitThrough(*epoch, now + kArrivalWait);
      }
      runtime.Pump();
      if (now - last_horizon >= kHorizonEvery) {
        last_horizon = now;
        slots.RefreshHorizon();
      }
      slots.EndMaintenance();
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
           gaps = 0, synced = 0, waited = 0, declined = 0;
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
    waited += segment->stats.waited;
    declined += segment->stats.declined;
    generation = std::max(generation, segment->generation.load());
  }
  char text[400];
  snprintf(text, sizeof(text),
           "requests=%llu served=%llu fallback=%llu errors=%llu commits=%llu applied=%llu "
           "builds=%llu gaps=%llu synced=%llu waited=%llu declined=%llu workers=%zu generation=%u pid=%u",
           (unsigned long long)requests, (unsigned long long)served, (unsigned long long)fallback,
           (unsigned long long)errors, (unsigned long long)commits, (unsigned long long)applied,
           (unsigned long long)builds, (unsigned long long)gaps, (unsigned long long)synced,
           (unsigned long long)waited, (unsigned long long)declined, running.size(), generation, running.front()->pid.load());
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
