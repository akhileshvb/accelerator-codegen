// Shared aux state server for PostgreSQL: see aux_server.hpp for the design.
#if defined(ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)

extern "C" {
#include <postgres.h>
#include <access/htup_details.h>
#include <access/tupdesc.h>
#include <access/xact.h>
#include <catalog/pg_type.h>
#include <commands/async.h>
#include <executor/executor.h>
#include <executor/tuptable.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/pg_list.h>
#include <nodes/execnodes.h>
#include <pgstat.h>
#include <port/atomics.h>
#include <postmaster/bgworker.h>
#include <postmaster/interrupt.h>
#include <storage/dsm.h>
#include <storage/dsm_registry.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/lock.h>
#include <storage/proc.h>
#include <storage/procnumber.h>
#include <storage/shm_mq.h>
#include <storage/spin.h>
#include <tcop/tcopprot.h>
#include <utils/builtins.h>
#include <utils/datum.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/snapmgr.h>
#include <utils/timestamp.h>
#include <utils/tuplestore.h>
}

#include "accelerator_codegen/stateful/postgres/aux_server.hpp"
#include "accelerator_codegen/stateful/postgres/aux_server_text.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_decision_log.hpp"
#include "accelerator_codegen/stateful/core/aux_maintenance_policy.hpp"
#include "accelerator_codegen/stateful/postgres/snapshot_token.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unistd.h>
#include <unordered_map>

namespace accelerator_codegen::aux_state::engine::postgres::aux_server {
namespace {

namespace auxsync = accelerator_codegen::aux_state::engine::postgres::aux_sync;

// ---- settings ---------------------------------------------------------------
bool g_shared_state = false;
int g_timeout_ms = 60000;       // a reader waits this long for the worker
int g_apply_timeout_ms = 5000;  // a committing writer waits this long
int g_idle_exit_s = 0;          // worker exits after this much idleness (0: never)

bool g_in_server = false;
bool g_log_decisions = false;

constexpr int kMaxClients = 256;
constexpr int kMaxRuntimes = 64;
constexpr int kMaxDeps = 8;
constexpr int kMaxAttnums = 64;
constexpr int kKeyLen = 64;
constexpr Size kMqBytes = 256 * 1024;
constexpr std::size_t kMaxShipRows = 500000;

enum : uint32 { kStopped = 0, kStarting = 1, kRunning = 2 };
enum Stat : int {
  kStatRequests,
  kStatHits,
  kStatRebuilds,
  kStatDeclined,
  kStatErrors,
  kStatApplies,
  kStatSynced,
  kStatUnsynced,
  kStatCount
};

struct SlotShm {
  pg_atomic_uint32 pid;
  pg_atomic_uint32 handle;
  pg_atomic_uint32 gen;
};
struct DepShm {
  uint64 table;
  int32 nattnums;
  int32 attnums[kMaxAttnums];
};
struct EntryShm {
  char key[kKeyLen];
  int32 ndeps;
  DepShm deps[kMaxDeps];
};
struct Shm {
  pg_atomic_uint32 state;
  pg_atomic_uint32 worker_pid;
  pg_atomic_uint32 worker_procno;
  pg_atomic_uint32 busy;
  pg_atomic_uint32 apply_inflight;
  pg_atomic_uint32 generation;
  pg_atomic_uint64 started_ms;
  pg_atomic_uint64 apply_since_ms;
  pg_atomic_uint64 skip_ship_until_ms;
  pg_atomic_uint64 stats[kStatCount];
  slock_t text_lock;
  char forced[48];
  char last_action[48];
  char last_decision[160];
  slock_t catalog_lock;
  pg_atomic_uint32 catalog_version;
  int32 nentries;
  EntryShm entries[kMaxRuntimes];
  SlotShm slots[kMaxClients];
};

void InitShm(void *ptr) {
  auto *shm = static_cast<Shm *>(ptr);
  std::memset(shm, 0, sizeof(Shm));
  pg_atomic_init_u32(&shm->state, kStopped);
  pg_atomic_init_u32(&shm->worker_pid, 0);
  pg_atomic_init_u32(&shm->worker_procno, 0);
  pg_atomic_init_u32(&shm->busy, 0);
  pg_atomic_init_u32(&shm->apply_inflight, 0);
  pg_atomic_init_u32(&shm->generation, 0);
  pg_atomic_init_u64(&shm->started_ms, 0);
  pg_atomic_init_u64(&shm->apply_since_ms, 0);
  pg_atomic_init_u64(&shm->skip_ship_until_ms, 0);
  for (int i = 0; i < kStatCount; ++i)
    pg_atomic_init_u64(&shm->stats[i], 0);
  SpinLockInit(&shm->catalog_lock);
  SpinLockInit(&shm->text_lock);
  pg_atomic_init_u32(&shm->catalog_version, 1);
  for (int i = 0; i < kMaxClients; ++i) {
    pg_atomic_init_u32(&shm->slots[i].pid, 0);
    pg_atomic_init_u32(&shm->slots[i].handle, 0);
    pg_atomic_init_u32(&shm->slots[i].gen, 0);
  }
}

Shm *g_shm = nullptr;
Oid g_shm_db = InvalidOid;

Shm *ShmFor(Oid db) {
  if (g_shm && g_shm_db == db)
    return g_shm;
  const std::string name =
      "accel_aux_srv_" + std::to_string(static_cast<unsigned>(db));
  bool found = false;
  g_shm = static_cast<Shm *>(
      GetNamedDSMSegment(name.c_str(), sizeof(Shm), InitShm, &found));
  g_shm_db = db;
  return g_shm;
}

int64_t NowMs() { return static_cast<int64_t>(GetCurrentTimestamp() / 1000); }

void Bump(Shm *shm, Stat s) { pg_atomic_fetch_add_u64(&shm->stats[s], 1); }

// ---- wire format ---------------------------------------------------------------
struct Out {
  std::string b;
  void u8(uint8_t v) { b.push_back(static_cast<char>(v)); }
  void raw(const void *p, std::size_t n) {
    b.append(static_cast<const char *>(p), n);
  }
  void u32(uint32_t v) { raw(&v, 4); }
  void u64(uint64_t v) { raw(&v, 8); }
  void i64(int64_t v) { raw(&v, 8); }
  void f64(double v) { raw(&v, 8); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    raw(s.data(), s.size());
  }
};

struct In {
  const char *p;
  std::size_t n;
  std::size_t off = 0;
  In(const char *p_p, std::size_t n_p) : p(p_p), n(n_p) {}
  void need(std::size_t k) const {
    if (off + k > n)
      throw std::runtime_error("aux server: truncated message");
  }
  uint8_t u8() { need(1); return static_cast<uint8_t>(p[off++]); }
  uint32_t u32() { uint32_t v; need(4); std::memcpy(&v, p + off, 4); off += 4; return v; }
  uint64_t u64() { uint64_t v; need(8); std::memcpy(&v, p + off, 8); off += 8; return v; }
  int64_t i64() { int64_t v; need(8); std::memcpy(&v, p + off, 8); off += 8; return v; }
  double f64() { double v; need(8); std::memcpy(&v, p + off, 8); off += 8; return v; }
  std::string str() {
    const uint32_t len = u32();
    need(len);
    std::string s(p + off, len);
    off += len;
    return s;
  }
  const char *bytes(std::size_t len) {
    need(len);
    const char *r = p + off;
    off += len;
    return r;
  }
};

void PutValue(Out &o, const core::AuxValue &v) {
  o.u8(static_cast<uint8_t>(v.type));
  o.u8(v.is_null ? 1 : 0);
  if (v.is_null)
    return;
  o.u8(static_cast<uint8_t>(v.payload.index()));
  std::visit(
      [&](const auto &x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, bool>) {
          o.u8(x ? 1 : 0);
        } else if constexpr (std::is_same_v<T, int64_t>) {
          o.i64(x);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          o.u64(x);
        } else if constexpr (std::is_same_v<T, core::ScaledDecimal>) {
          o.i64(x.value);
          o.u32(static_cast<uint32_t>(x.scale));
        } else if constexpr (std::is_same_v<T, double>) {
          o.f64(x);
        } else if constexpr (std::is_same_v<T, core::AuxDate>) {
          o.u32(static_cast<uint32_t>(x.days_since_epoch));
        } else {
          o.str(x);
        }
      },
      v.payload);
}

core::AuxValue GetValue(In &in) {
  core::AuxValue v;
  v.type = static_cast<core::AuxType>(in.u8());
  v.is_null = in.u8() != 0;
  if (v.is_null)
    return v;
  switch (in.u8()) {
  case 0: v.payload = (in.u8() != 0); break;
  case 1: v.payload = in.i64(); break;
  case 2: v.payload = in.u64(); break;
  case 3: {
    core::ScaledDecimal d;
    d.value = in.i64();
    d.scale = static_cast<int32_t>(in.u32());
    v.payload = d;
    break;
  }
  case 4: v.payload = in.f64(); break;
  case 5: {
    core::AuxDate d;
    d.days_since_epoch = static_cast<int32_t>(in.u32());
    v.payload = d;
    break;
  }
  default: v.payload = in.str(); break;
  }
  return v;
}

void PutRows(Out &o, const std::vector<core::AuxRow> &rows) {
  o.u32(static_cast<uint32_t>(rows.size()));
  for (const auto &row : rows) {
    o.u32(static_cast<uint32_t>(row.values.size()));
    for (const auto &v : row.values)
      PutValue(o, v);
  }
}

void PutBatch(Out &o, const core::MutationBatch &b) {
  o.u64(b.table);
  o.u8(static_cast<uint8_t>(b.kind));
  PutRows(o, b.old_rows);
  PutRows(o, b.new_rows);
  o.u32(static_cast<uint32_t>(b.updated_columns.size()));
  for (auto c : b.updated_columns)
    o.u32(c);
}

std::vector<core::AuxRow> ReadRows(In &in) {
  std::vector<core::AuxRow> rows;
  const uint32_t n = in.u32();
  rows.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    core::AuxRow row;
    const uint32_t nv = in.u32();
    row.values.reserve(nv);
    for (uint32_t j = 0; j < nv; ++j)
      row.values.push_back(GetValue(in));
    rows.push_back(std::move(row));
  }
  return rows;
}

core::MutationBatch GetBatch(In &in) {
  core::MutationBatch b;
  b.table = in.u64();
  b.kind = static_cast<core::MutationKind>(in.u8());
  b.old_rows = ReadRows(in);
  b.new_rows = ReadRows(in);
  const uint32_t nc = in.u32();
  for (uint32_t i = 0; i < nc; ++i)
    b.updated_columns.push_back(in.u32());
  return b;
}

// ---- mq helpers (never raise unless `interruptible`) -------------------------------
enum class MqResult { Ok, Detached, Timeout };

MqResult SendAll(shm_mq_handle *mqh, const std::string &msg, int64_t deadline_ms,
                 bool interruptible) {
  for (;;) {
    const shm_mq_result r = shm_mq_send(mqh, msg.size(), msg.data(), true, true);
    if (r == SHM_MQ_SUCCESS)
      return MqResult::Ok;
    if (r == SHM_MQ_DETACHED)
      return MqResult::Detached;
    if (NowMs() >= deadline_ms)
      return MqResult::Timeout;
    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 5,
                    PG_WAIT_EXTENSION);
    ResetLatch(MyLatch);
    if (interruptible)
      CHECK_FOR_INTERRUPTS();
  }
}

MqResult RecvOne(shm_mq_handle *mqh, std::string &out, int64_t deadline_ms,
                 bool interruptible) {
  for (;;) {
    Size n = 0;
    void *data = nullptr;
    const shm_mq_result r = shm_mq_receive(mqh, &n, &data, true);
    if (r == SHM_MQ_SUCCESS) {
      out.assign(static_cast<const char *>(data), n);
      return MqResult::Ok;
    }
    if (r == SHM_MQ_DETACHED)
      return MqResult::Detached;
    if (NowMs() >= deadline_ms)
      return MqResult::Timeout;
    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 5,
                    PG_WAIT_EXTENSION);
    ResetLatch(MyLatch);
    if (interruptible)
      CHECK_FOR_INTERRUPTS();
  }
}

// =================================================================================
// Reader / writer side
// =================================================================================
struct Chan {
  dsm_segment *seg = nullptr;
  shm_mq_handle *req = nullptr; // backend -> worker
  shm_mq_handle *rsp = nullptr; // worker -> backend
  int slot = -1;
  uint32 gen = 0;
  Oid db = InvalidOid;
  uint32 worker_generation = 0;
};
Chan g_chan;
bool g_exit_hook = false;
int64_t g_backoff_until_ms = 0;

void ReleaseSlot(Shm *shm, int slot) {
  if (slot < 0)
    return;
  uint32 expected = static_cast<uint32>(MyProcPid);
  pg_atomic_write_u32(&shm->slots[slot].handle, 0);
  pg_atomic_compare_exchange_u32(&shm->slots[slot].pid, &expected, 0);
}

void CloseChannel() {
  if (g_chan.seg == nullptr && g_chan.slot < 0)
    return;
  Shm *shm = g_shm;
  if (shm && g_chan.slot >= 0)
    ReleaseSlot(shm, g_chan.slot);
  if (g_chan.seg)
    dsm_detach(g_chan.seg); // detaches the mqs (the worker sees it)
  g_chan = Chan{};
}

void ClientExit(int, Datum) {
  // Only the slot needs releasing: dsm detach runs by itself at exit.
  if (g_shm && g_chan.slot >= 0)
    ReleaseSlot(g_shm, g_chan.slot);
  g_chan.slot = -1;
}

bool WorkerRunning(Shm *shm) {
  if (pg_atomic_read_u32(&shm->state) != kRunning)
    return false;
  const pid_t pid = static_cast<pid_t>(pg_atomic_read_u32(&shm->worker_pid));
  return pid > 0 && !(kill(pid, 0) != 0 && errno == ESRCH);
}

std::string ModulePath() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(&accel_aux_server_main), &info) &&
      info.dli_fname && *info.dli_fname)
    return info.dli_fname;
  return "accelerator_codegen_pg";
}

// Starts this database's worker if it is not running.
bool EnsureWorker(Shm *shm) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint32 st = pg_atomic_read_u32(&shm->state);
    if (st == kRunning) {
      if (WorkerRunning(shm))
        return true;
      uint32 expected = kRunning;
      pg_atomic_compare_exchange_u32(&shm->state, &expected, kStopped);
      continue;
    }
    if (st == kStarting) {
      const int64_t until = NowMs() + 20000;
      while (NowMs() < until && pg_atomic_read_u32(&shm->state) == kStarting) {
        (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                        20, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
        CHECK_FOR_INTERRUPTS();
      }
      if (pg_atomic_read_u32(&shm->state) == kRunning)
        return WorkerRunning(shm);
      if (NowMs() - static_cast<int64_t>(pg_atomic_read_u64(&shm->started_ms)) >
          20000) {
        uint32 expected = kStarting;
        pg_atomic_compare_exchange_u32(&shm->state, &expected, kStopped);
        continue;
      }
      return false;
    }
    uint32 expected = kStopped;
    if (!pg_atomic_compare_exchange_u32(&shm->state, &expected, kStarting))
      continue;
    pg_atomic_write_u64(&shm->started_ms, NowMs());
    BackgroundWorker bgw;
    std::memset(&bgw, 0, sizeof(bgw));
    bgw.bgw_flags =
        BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    bgw.bgw_start_time = BgWorkerStart_ConsistentState;
    bgw.bgw_restart_time = BGW_NEVER_RESTART;
    const std::string lib = ModulePath();
    snprintf(bgw.bgw_library_name, BGW_MAXLEN, "%s", lib.c_str());
    snprintf(bgw.bgw_function_name, BGW_MAXLEN, "accel_aux_server_main");
    snprintf(bgw.bgw_name, BGW_MAXLEN, "accel aux server (database %u)",
             static_cast<unsigned>(MyDatabaseId));
    snprintf(bgw.bgw_type, BGW_MAXLEN, "accel aux server");
    bgw.bgw_main_arg = ObjectIdGetDatum(MyDatabaseId);
    bgw.bgw_notify_pid = MyProcPid;
    BackgroundWorkerHandle *handle = nullptr;
    if (!RegisterDynamicBackgroundWorker(&bgw, &handle)) {
      pg_atomic_write_u32(&shm->state, kStopped);
      return false;
    }
    pid_t pid = 0;
    if (WaitForBackgroundWorkerStartup(handle, &pid) != BGWH_STARTED) {
      uint32 e2 = kStarting;
      pg_atomic_compare_exchange_u32(&shm->state, &e2, kStopped);
      return false;
    }
    const int64_t until = NowMs() + 20000;
    while (NowMs() < until && pg_atomic_read_u32(&shm->state) == kStarting) {
      (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                      10, PG_WAIT_EXTENSION);
      ResetLatch(MyLatch);
      CHECK_FOR_INTERRUPTS();
    }
    return pg_atomic_read_u32(&shm->state) == kRunning;
  }
  return false;
}

// Makes sure this backend has a channel to the running worker.  `start`:
// start the worker if needed (readers do; writers only use a running one).
bool EnsureChannel(bool start) {
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm)
    return false;
  if (g_chan.seg && g_chan.db == MyDatabaseId && WorkerRunning(shm) &&
      g_chan.worker_generation == pg_atomic_read_u32(&shm->generation))
    return true;
  CloseChannel();
  if (!WorkerRunning(shm)) {
    if (!start || !EnsureWorker(shm))
      return false;
  }
  if (!g_exit_hook) {
    before_shmem_exit(ClientExit, 0);
    g_exit_hook = true;
  }
  dsm_segment *seg = dsm_create(2 * kMqBytes, DSM_CREATE_NULL_IF_MAXSEGMENTS);
  if (!seg)
    return false;
  dsm_pin_mapping(seg);
  char *base = static_cast<char *>(dsm_segment_address(seg));
  shm_mq *rq = shm_mq_create(base, kMqBytes);
  shm_mq *sq = shm_mq_create(base + kMqBytes, kMqBytes);
  shm_mq_set_sender(rq, MyProc);
  shm_mq_set_receiver(sq, MyProc);
  g_chan.seg = seg;
  // The handles outlive the statement that creates them.
  MemoryContext old_ctx = MemoryContextSwitchTo(TopMemoryContext);
  g_chan.req = shm_mq_attach(rq, seg, nullptr);
  g_chan.rsp = shm_mq_attach(sq, seg, nullptr);
  MemoryContextSwitchTo(old_ctx);
  g_chan.db = MyDatabaseId;
  g_chan.worker_generation = pg_atomic_read_u32(&shm->generation);
  int slot = -1;
  for (int i = 0; i < kMaxClients && slot < 0; ++i) {
    uint32 expected = 0;
    if (pg_atomic_compare_exchange_u32(&shm->slots[i].pid, &expected,
                                       static_cast<uint32>(MyProcPid)))
      slot = i;
  }
  if (slot < 0) {
    CloseChannel();
    return false;
  }
  g_chan.slot = slot;
  g_chan.gen = pg_atomic_add_fetch_u32(&shm->slots[slot].gen, 1);
  pg_memory_barrier();
  pg_atomic_write_u32(&shm->slots[slot].handle, dsm_segment_handle(seg));
  pg_memory_barrier();
  ProcSendSignal(static_cast<ProcNumber>(pg_atomic_read_u32(&shm->worker_procno)));
  return true;
}

// ---- catalog (writer side cache) ----------------------------------------------------
struct CatalogCopy {
  std::string key;
  std::vector<std::pair<uint64_t, std::vector<int32_t>>> deps;
};
std::vector<CatalogCopy> g_catalog;
uint32 g_catalog_version = 0;
Oid g_catalog_db = InvalidOid;

void RefreshCatalog(Shm *shm) {
  const uint32 v = pg_atomic_read_u32(&shm->catalog_version);
  if (g_catalog_db == MyDatabaseId && v == g_catalog_version)
    return;
  std::vector<CatalogCopy> copy;
  uint32 version;
  SpinLockAcquire(&shm->catalog_lock);
  version = pg_atomic_read_u32(&shm->catalog_version);
  for (int i = 0; i < shm->nentries; ++i) {
    const EntryShm &e = shm->entries[i];
    CatalogCopy c;
    c.key = e.key;
    for (int d = 0; d < e.ndeps; ++d) {
      std::vector<int32_t> attnums(e.deps[d].attnums,
                                   e.deps[d].attnums + e.deps[d].nattnums);
      c.deps.emplace_back(e.deps[d].table, std::move(attnums));
    }
    copy.push_back(std::move(c));
  }
  SpinLockRelease(&shm->catalog_lock);
  g_catalog = std::move(copy);
  g_catalog_version = version;
  g_catalog_db = MyDatabaseId;
}

// ---- writer staging ------------------------------------------------------------------
struct StagedBatch {
  std::string key;
  core::MutationBatch batch;
};
struct Stage {
  std::vector<std::vector<StagedBatch>> frames;
  std::map<std::string, std::set<std::size_t>> examined;
  std::set<std::string> poisoned;
  std::string payload;
  uint32_t entries = 0;
  bool prepared = false;
  bool inflight_marked = false;
};
Stage g_stage;

std::size_t RowCount(const core::MutationBatch &b) {
  return b.old_rows.size() + b.new_rows.size();
}

// =================================================================================
// Worker side
// =================================================================================
std::vector<ServerRuntime *> g_runtimes;
ServerRuntime *g_current_runtime = nullptr;
bool g_declined = false;
bool g_served = false;
core::AuxExecutePath g_path = core::AuxExecutePath::Hit;
std::string g_state_path; // search_path the state was built under

struct WChan {
  dsm_segment *seg = nullptr;
  shm_mq_handle *req = nullptr;
  shm_mq_handle *rsp = nullptr;
  uint32 handle = 0;
  uint32 gen = 0;
};
WChan g_wchans[kMaxClients];

void DropWChan(Shm *shm, int slot, bool clear_slot) {
  WChan &c = g_wchans[slot];
  if (c.seg)
    dsm_detach(c.seg);
  c = WChan{};
  if (clear_slot) {
    // Free the slot if its owner is gone (a live owner releases it itself).
    const uint32 pid = pg_atomic_read_u32(&shm->slots[slot].pid);
    if (pid != 0 && kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
      uint32 expected = pid;
      pg_atomic_write_u32(&shm->slots[slot].handle, 0);
      pg_atomic_compare_exchange_u32(&shm->slots[slot].pid, &expected, 0);
    }
  }
}

void PollSlots(Shm *shm) {
  for (int i = 0; i < kMaxClients; ++i) {
    const uint32 pid = pg_atomic_read_u32(&shm->slots[i].pid);
    pg_memory_barrier();
    const uint32 handle = pg_atomic_read_u32(&shm->slots[i].handle);
    const uint32 gen = pg_atomic_read_u32(&shm->slots[i].gen);
    WChan &c = g_wchans[i];
    if (pid == 0 || handle == 0) {
      if (c.seg)
        DropWChan(shm, i, false);
      continue;
    }
    if (c.seg && c.handle == handle && c.gen == gen)
      continue;
    if (c.seg)
      DropWChan(shm, i, false);
    dsm_segment *seg = dsm_attach(handle);
    if (!seg)
      continue;
    dsm_pin_mapping(seg);
    char *base = static_cast<char *>(dsm_segment_address(seg));
    shm_mq *rq = reinterpret_cast<shm_mq *>(base);
    shm_mq *sq = reinterpret_cast<shm_mq *>(base + kMqBytes);
    shm_mq_set_receiver(rq, MyProc);
    shm_mq_set_sender(sq, MyProc);
    c.seg = seg;
    MemoryContext old_ctx = MemoryContextSwitchTo(TopMemoryContext);
    c.req = shm_mq_attach(rq, seg, nullptr);
    c.rsp = shm_mq_attach(sq, seg, nullptr);
    MemoryContextSwitchTo(old_ctx);
    c.handle = handle;
    c.gen = gen;
  }
}

bool Reply(Shm *shm, int slot, const std::string &msg) {
  WChan &c = g_wchans[slot];
  if (!c.rsp)
    return false;
  const shm_mq_result r = shm_mq_send(c.rsp, msg.size(), msg.data(), false, true);
  if (r != SHM_MQ_SUCCESS) {
    DropWChan(shm, slot, true);
    return false;
  }
  return true;
}

void ReplyError(Shm *shm, int slot, const std::string &text) {
  Out o;
  o.u8('E');
  o.str(text);
  Reply(shm, slot, o.b);
}

void BeginRequestTxn() {
  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  (void)GetTransactionSnapshot();
  ClearSnapshotPins();
}

void HandleQuery(Shm *shm, int slot, const std::string &msg) {
  Bump(shm, kStatRequests);
  In in(msg.data(), msg.size());
  in.u8();
  const Oid fn_oid = in.u32();
  const Oid collation = in.u32();
  const std::string search_path = in.str();
  const uint32_t nargs = in.u32();

  BeginRequestTxn();
  MemoryContext req_ctx = AllocSetContextCreate(
      CurrentMemoryContext, "accel aux request", ALLOCSET_DEFAULT_SIZES);
  MemoryContext old_ctx = MemoryContextSwitchTo(req_ctx);

  bool failed = false;
  std::string error;
  Out reply;
  bool replied = false;
  try {
    if (g_state_path.empty())
      g_state_path = search_path;
    if (search_path != g_state_path) {
      // The state was built for another search_path (other tables, maybe).
      g_declined = true;
    } else {
      FmgrInfo flinfo;
      fmgr_info(fn_oid, &flinfo);
      {
        // Call-expression introspection (get_fn_expr_argtype and friends)
        // works as in the caller: typed placeholders for the arguments.
        Oid *argtypes = nullptr;
        int declared = 0;
        const Oid rettype = get_func_signature(fn_oid, &argtypes, &declared);
        List *args = NIL;
        for (int i = 0; i < declared; ++i)
          args = lappend(args, makeNullConst(argtypes[i], -1, InvalidOid));
        flinfo.fn_expr = reinterpret_cast<fmNodePtr>(makeFuncExpr(
            fn_oid, rettype, args, InvalidOid, collation, COERCE_EXPLICIT_CALL));
      }
      FunctionCallInfo fc = static_cast<FunctionCallInfo>(
          palloc0(SizeForFunctionCallInfo(nargs ? nargs : 1)));
      ReturnSetInfo rsinfo;
      std::memset(&rsinfo, 0, sizeof(rsinfo));
      ExprContext *econtext = CreateStandaloneExprContext();
      rsinfo.type = T_ReturnSetInfo;
      rsinfo.econtext = econtext;
      rsinfo.allowedModes = static_cast<int>(SFRM_ValuePerCall | SFRM_Materialize);
      rsinfo.returnMode = SFRM_ValuePerCall;
      InitFunctionCallInfoData(*fc, &flinfo, static_cast<int>(nargs), collation,
                               nullptr, reinterpret_cast<Node *>(&rsinfo));
      for (uint32_t i = 0; i < nargs; ++i) {
        const uint32_t len = in.u32();
        const char *p = in.bytes(len);
        char *copy = static_cast<char *>(palloc(len ? len : 1));
        std::memcpy(copy, p, len);
        bool isnull = false;
        char *cursor = copy;
        fc->args[i].value = datumRestore(&cursor, &isnull);
        fc->args[i].isnull = isnull;
      }
      const uint32_t snaplen = in.u32();
      const char *snapbytes = in.bytes(snaplen);
      char *snapcopy = static_cast<char *>(palloc(snaplen ? snaplen : 1));
      std::memcpy(snapcopy, snapbytes, snaplen);
      Snapshot snap = RestoreSnapshot(snapcopy);
      PushActiveSnapshot(snap);

      g_declined = false;
      g_served = false;
      g_current_runtime = nullptr;
      (void)FunctionCallInvoke(fc);

      PopActiveSnapshot();
      if (!g_declined && !g_served) {
        // The function did not go through a Runtime: nothing to serve.
        g_declined = true;
      }
      if (!g_declined) {
        if (rsinfo.returnMode != SFRM_Materialize || rsinfo.setResult == nullptr ||
            rsinfo.setDesc == nullptr)
          throw std::runtime_error("aux server: function did not materialize");
        TupleTableSlot *tslot =
            MakeSingleTupleTableSlot(rsinfo.setDesc, &TTSOpsMinimalTuple);
        Out batch;
        uint32_t in_batch = 0;
        std::string head;
        auto flush = [&]() {
          if (in_batch == 0)
            return;
          Out m;
          m.u8('T');
          m.u32(in_batch);
          m.b += batch.b;
          Reply(shm, slot, m.b);
          batch.b.clear();
          in_batch = 0;
        };
        while (tuplestore_gettupleslot(rsinfo.setResult, true, false, tslot)) {
          bool shouldfree = false;
          MinimalTuple mt = ExecFetchSlotMinimalTuple(tslot, &shouldfree);
          batch.u32(mt->t_len);
          batch.raw(mt, mt->t_len);
          ++in_batch;
          if (batch.b.size() > (1u << 20))
            flush();
          if (shouldfree)
            pfree(mt);
        }
        flush();
        ExecDropSingleTupleTableSlot(tslot);
      }
      FreeExprContext(econtext, true);
    }
    reply.u8(g_declined ? 'F' : 'D');
    if (!g_declined)
      reply.u8(static_cast<uint8_t>(g_path));
    replied = true;
  } catch (const std::exception &ex) {
    failed = true;
    error = ex.what();
  }

  MemoryContextSwitchTo(old_ctx);
  if (failed) {
    if (g_current_runtime)
      g_current_runtime->ServerInvalidate();
    g_current_runtime = nullptr;
    AbortCurrentTransaction();
    Bump(shm, kStatErrors);
    ReplyError(shm, slot, error);
    return;
  }
  MemoryContextDelete(req_ctx);
  CommitTransactionCommand();
  if (replied) {
    if (g_declined)
      Bump(shm, kStatDeclined);
    else if (g_path == core::AuxExecutePath::RebuiltOnRead)
      Bump(shm, kStatRebuilds);
    else
      Bump(shm, kStatHits);
    Reply(shm, slot, reply.b);
  }
}

void HandleApply(Shm *shm, int slot, const std::string &msg) {
  In in(msg.data(), msg.size());
  in.u8();
  ApplyInfo info;
  info.xid = in.u32();
  const uint32_t nwrote = in.u32();
  for (uint32_t i = 0; i < nwrote; ++i) {
    const uint32_t s = in.u32();
    const uint64_t seq = in.u64();
    if (s < auxsync::kSlots) {
      info.wrote[s] = true;
      info.old_seq[s] = seq;
    }
  }
  std::map<std::string, ApplyEntry> entries;
  const uint32_t nentries = in.u32();
  for (uint32_t i = 0; i < nentries; ++i) {
    const std::string key = in.str();
    ApplyEntry e;
    const uint32_t ns = in.u32();
    for (uint32_t j = 0; j < ns; ++j)
      e.examined_slots.push_back(in.u32());
    const uint32_t nb = in.u32();
    for (uint32_t j = 0; j < nb; ++j)
      e.batches.push_back(GetBatch(in));
    entries.emplace(key, std::move(e));
  }

  Bump(shm, kStatApplies);
  {
    char forced[48];
    SpinLockAcquire(&shm->text_lock);
    std::memcpy(forced, shm->forced, sizeof(forced));
    SpinLockRelease(&shm->text_lock);
    forced[sizeof(forced) - 1] = '\0';
    core::SetForcedMaintenanceActionName(forced);
    core::ClearMaintenanceDecisionLog();
  }
  BeginRequestTxn();
  PushActiveSnapshot(GetLatestSnapshot());
  bool failed = false;
  try {
    for (auto *runtime : g_runtimes) {
      auto it = entries.find(runtime->ServerKey());
      try {
        runtime->ServerApply(info, it == entries.end() ? nullptr : &it->second);
      } catch (const std::exception &) {
        runtime->ServerInvalidate();
      }
    }
  } catch (...) {
    failed = true;
  }
  if (failed) {
    AbortCurrentTransaction();
  } else {
    PopActiveSnapshot();
    CommitTransactionCommand();
  }
  {
    // Publish the last maintenance decision for the diagnostics functions.
    const auto decision = core::LastMaintenanceDecision();
    std::string name, text;
    if (decision && decision->valid) {
      name = maintenance::MaintenanceActionName(decision->staged_action);
      text = core::FormatLastMaintenanceDecision();
    }
    SpinLockAcquire(&shm->text_lock);
    snprintf(shm->last_action, sizeof(shm->last_action), "%s", name.c_str());
    snprintf(shm->last_decision, sizeof(shm->last_decision), "%s", text.c_str());
    SpinLockRelease(&shm->text_lock);
  }
  Out ack;
  ack.u8('A');
  Reply(shm, slot, ack.b);
}

void WorkerExit(int, Datum arg) {
  Shm *shm = g_shm;
  if (!shm)
    return;
  pg_atomic_write_u32(&shm->busy, 0);
  pg_atomic_write_u32(&shm->worker_pid, 0);
  pg_atomic_write_u32(&shm->state, kStopped);
}

} // namespace

// =================================================================================
// Public API
// =================================================================================
void DefineGucs() {
  DefineCustomBoolVariable(
      "accel.shared_state",
      "Share accelerator state across connections through a per-database "
      "background worker.",
      nullptr, &g_shared_state, false, PGC_USERSET, 0, nullptr, nullptr, nullptr);
  DefineCustomBoolVariable(
      "accel.log_decisions",
      "Log each accelerator's hit / rebuild / fallback decision (diagnostics).",
      nullptr, &g_log_decisions, false, PGC_USERSET, 0, nullptr, nullptr, nullptr);
  DefineCustomIntVariable(
      "accel.shared_timeout_ms",
      "How long a query waits for the shared state worker before it answers "
      "from SQL.",
      nullptr, &g_timeout_ms, 60000, 100, 3600000, PGC_USERSET, 0, nullptr,
      nullptr, nullptr);
  DefineCustomIntVariable(
      "accel.shared_apply_timeout_ms",
      "How long a committing writer waits for the shared state worker to apply "
      "its changes.",
      nullptr, &g_apply_timeout_ms, 5000, 10, 600000, PGC_USERSET, 0, nullptr,
      nullptr, nullptr);
  DefineCustomIntVariable(
      "accel.shared_idle_exit_s",
      "The shared state worker exits after this many idle seconds (0: never).",
      nullptr, &g_idle_exit_s, 0, 0, 86400, PGC_SIGHUP, 0, nullptr, nullptr,
      nullptr);
}

bool InServer() { return g_in_server; }
bool LogDecisions() { return g_log_decisions; }
bool SharedEnabled() { return g_shared_state && !g_in_server; }

void RegisterServerRuntime(ServerRuntime *runtime) {
  if (std::find(g_runtimes.begin(), g_runtimes.end(), runtime) != g_runtimes.end())
    return;
  g_runtimes.push_back(runtime);
  Shm *shm = g_shm;
  if (!shm)
    return;
  const auto deps = runtime->ServerDependencies();
  EntryShm entry;
  std::memset(&entry, 0, sizeof(entry));
  snprintf(entry.key, kKeyLen, "%s", runtime->ServerKey().c_str());
  std::set<uint64_t> seen;
  for (const auto &dep : deps) {
    if (entry.ndeps >= kMaxDeps || !seen.insert(dep.table).second)
      continue;
    DepShm &d = entry.deps[entry.ndeps++];
    d.table = dep.table;
    d.nattnums = static_cast<int32>(
        std::min<std::size_t>(dep.mutation_attnums.size(), kMaxAttnums));
    for (int32 i = 0; i < d.nattnums; ++i)
      d.attnums[i] = dep.mutation_attnums[i];
  }
  SpinLockAcquire(&shm->catalog_lock);
  if (shm->nentries < kMaxRuntimes) {
    std::memcpy(&shm->entries[shm->nentries], &entry, sizeof(entry));
    ++shm->nentries;
    pg_atomic_fetch_add_u32(&shm->catalog_version, 1);
  }
  SpinLockRelease(&shm->catalog_lock);
}

void NoteServed(core::AuxExecutePath path) {
  g_served = true;
  g_declined = false;
  g_path = path;
}
void NoteDeclined() {
  g_declined = true;
  g_served = false;
}
void NoteCurrentRuntime(ServerRuntime *runtime) { g_current_runtime = runtime; }

BusyScope::BusyScope() : active(g_in_server && g_shm != nullptr) {
  if (active)
    pg_atomic_write_u32(&g_shm->busy, 1);
}
BusyScope::~BusyScope() {
  if (active)
    pg_atomic_write_u32(&g_shm->busy, 0);
}

// ---- reader ---------------------------------------------------------------------------
Remote RemoteExecute(FunctionCallInfo fcinfo, core::AuxExecutePath &path) {
  if (!SharedEnabled() || fcinfo == nullptr || fcinfo->flinfo == nullptr)
    return Remote::Unavailable;
  if (!ActiveSnapshotSet())
    return Remote::Unavailable;
  Snapshot snapshot = GetActiveSnapshot();
  if (!auxsync::UsableSnapshot(snapshot))
    return Remote::Unavailable;
  if (NowMs() < g_backoff_until_ms)
    return Remote::Unavailable;

  Oid *argtypes = nullptr;
  int declared = 0;
  (void)get_func_signature(fcinfo->flinfo->fn_oid, &argtypes, &declared);
  if (declared != fcinfo->nargs)
    return Remote::Unavailable;

  Out req;
  req.u8('Q');
  req.u32(fcinfo->flinfo->fn_oid);
  req.u32(fcinfo->fncollation);
  const char *sp = GetConfigOption("search_path", false, false);
  req.str(sp ? sp : "");
  req.u32(static_cast<uint32_t>(fcinfo->nargs));
  for (int i = 0; i < fcinfo->nargs; ++i) {
    int16 typlen = 0;
    bool typbyval = false;
    get_typlenbyval(argtypes[i], &typlen, &typbyval);
    Datum value = fcinfo->args[i].value;
    const bool isnull = fcinfo->args[i].isnull;
    if (!isnull && typlen == -1)
      value = PointerGetDatum(pg_detoast_datum_packed((struct varlena *)DatumGetPointer(value)));
    const Size space = datumEstimateSpace(value, isnull, typbyval, typlen);
    std::string buf(space, '\0');
    char *cursor = &buf[0];
    datumSerialize(value, isnull, typbyval, typlen, &cursor);
    req.u32(static_cast<uint32_t>(space));
    req.raw(buf.data(), space);
  }
  const Size snapspace = EstimateSnapshotSpace(snapshot);
  std::string snapbuf(snapspace, '\0');
  SerializeSnapshot(snapshot, &snapbuf[0]);
  req.u32(static_cast<uint32_t>(snapspace));
  req.raw(snapbuf.data(), snapspace);

  const int64_t deadline = NowMs() + g_timeout_ms;
  std::vector<std::string> tuples;
  Remote result = Remote::Unavailable;
  bool close = false;

  PG_TRY();
  {
    if (!EnsureChannel(true)) {
      g_backoff_until_ms = NowMs() + 2000;
    } else if (SendAll(g_chan.req, req.b, deadline, true) != MqResult::Ok) {
      close = true;
    } else {
      for (;;) {
        std::string msg;
        const MqResult r = RecvOne(g_chan.rsp, msg, deadline, true);
        if (r != MqResult::Ok) {
          close = true;
          if (r == MqResult::Detached)
            g_backoff_until_ms = NowMs() + 500;
          break;
        }
        In in(msg.data(), msg.size());
        const uint8_t kind = in.u8();
        if (kind == 'T') {
          const uint32_t n = in.u32();
          for (uint32_t i = 0; i < n; ++i) {
            const uint32_t len = in.u32();
            tuples.emplace_back(in.bytes(len), len);
          }
        } else if (kind == 'D') {
          path = static_cast<core::AuxExecutePath>(in.u8());
          result = Remote::Served;
          break;
        } else if (kind == 'F') {
          result = Remote::Declined;
          break;
        } else {
          // 'E' (worker-side failure) or garbage: answer from SQL.
          elog(DEBUG1, "accelerator shared state worker failed: %s",
               kind == 'E' ? in.str().c_str() : "bad reply");
          break;
        }
      }
    }
  }
  PG_CATCH();
  {
    CloseChannel();
    PG_RE_THROW();
  }
  PG_END_TRY();

  if (close)
    CloseChannel();
  if (result == Remote::Served) {
    auto *rsinfo = reinterpret_cast<ReturnSetInfo *>(fcinfo->resultinfo);
    Tuplestorestate *store = rsinfo->setResult;
    for (const auto &bytes : tuples) {
      MinimalTuple mt = static_cast<MinimalTuple>(palloc(bytes.size()));
      std::memcpy(mt, bytes.data(), bytes.size());
      HeapTuple ht = heap_tuple_from_minimal_tuple(mt);
      tuplestore_puttuple(store, ht);
      heap_freetuple(ht);
      pfree(mt);
    }
  }
  return result;
}

// ---- writer ---------------------------------------------------------------------------
std::vector<CaptureTarget> CaptureTargets(core::TableId table) {
  std::vector<CaptureTarget> out;
  if (!SharedEnabled())
    return out;
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm || pg_atomic_read_u32(&shm->state) != kRunning)
    return out;
  RefreshCatalog(shm);
  for (const auto &entry : g_catalog) {
    for (const auto &dep : entry.deps) {
      if (dep.first == table) {
        out.push_back(CaptureTarget{entry.key, dep.second});
        break;
      }
    }
  }
  return out;
}

void StageExamined(const std::string &key, std::size_t slot) {
  g_stage.examined[key].insert(slot);
}

void StageBatch(const std::string &key, core::MutationBatch batch,
                std::size_t depth) {
  if (g_stage.frames.size() <= depth)
    g_stage.frames.resize(depth + 1);
  g_stage.frames[depth].push_back(StagedBatch{key, std::move(batch)});
}

void OnSubCommit(std::size_t depth) {
  if (depth == 0 || g_stage.frames.size() <= depth)
    return;
  // Merge this frame (and any deeper leftovers) into its parent.
  auto &parent = g_stage.frames[depth - 1];
  for (std::size_t d = depth; d < g_stage.frames.size(); ++d) {
    for (auto &b : g_stage.frames[d])
      parent.push_back(std::move(b));
  }
  g_stage.frames.resize(depth);
}

void OnSubAbort(std::size_t depth) {
  if (depth == 0 || g_stage.frames.size() <= depth)
    return;
  // The aborted statements never happened, but a later statement's lookups
  // could have seen their rows: those accelerators are not shipped.
  for (std::size_t d = depth; d < g_stage.frames.size(); ++d)
    for (const auto &b : g_stage.frames[d])
      g_stage.poisoned.insert(b.key);
  g_stage.frames.resize(depth);
}

void PrepareCommit() {
  g_stage.prepared = false;
  g_stage.payload.clear();
  g_stage.entries = 0;
  if (!SharedEnabled() || g_stage.examined.empty())
    return;
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm || !WorkerRunning(shm) || NowMs() < static_cast<int64_t>(pg_atomic_read_u64(&shm->skip_ship_until_ms)) ||
      pg_atomic_read_u32(&shm->busy) != 0)
    return;

  // Per accelerator: its batches in order; ship only when there is at most
  // one (see the design note) and nothing was poisoned.
  std::map<std::string, std::vector<const core::MutationBatch *>> per_key;
  for (const auto &frame : g_stage.frames)
    for (const auto &b : frame)
      per_key[b.key].push_back(&b.batch);
  Out payload;
  uint32_t entries = 0;
  for (const auto &kv : g_stage.examined) {
    const std::string &key = kv.first;
    if (g_stage.poisoned.count(key))
      continue;
    const auto it = per_key.find(key);
    const std::size_t nbatches = it == per_key.end() ? 0 : it->second.size();
    if (nbatches > 1)
      continue;
    if (nbatches == 1 && RowCount(*it->second[0]) > kMaxShipRows)
      continue;
    payload.str(key);
    payload.u32(static_cast<uint32_t>(kv.second.size()));
    for (auto s : kv.second)
      payload.u32(static_cast<uint32_t>(s));
    payload.u32(static_cast<uint32_t>(nbatches));
    if (nbatches == 1)
      PutBatch(payload, *it->second[0]);
    ++entries;
  }
  if (entries == 0)
    return;
  if (!EnsureChannel(false))
    return;
  auxsync::AcquireCommitGate();
  // The wait for the gate may have been long: do not queue behind a worker
  // that has become busy or unresponsive meanwhile (the gate stays held).
  if (!WorkerRunning(shm) || pg_atomic_read_u32(&shm->busy) != 0 ||
      NowMs() < static_cast<int64_t>(pg_atomic_read_u64(&shm->skip_ship_until_ms)))
    return;
  pg_atomic_fetch_add_u32(&shm->apply_inflight, 1);
  pg_atomic_write_u64(&shm->apply_since_ms, NowMs());
  g_stage.inflight_marked = true;
  g_stage.payload = std::move(payload.b);
  g_stage.entries = entries;
  g_stage.prepared = true;
}

void ShipCommit(const aux_sync::CommitInfo &info) {
  if (!g_stage.prepared)
    return;
  Shm *shm = g_shm;
  bool acked = false;
  try {
    if (shm && g_chan.seg && TransactionIdIsValid(info.xid)) {
      Out msg;
      msg.u8('A');
      msg.u32(info.xid);
      uint32_t nwrote = 0;
      for (std::size_t k = 0; k < auxsync::kSlots; ++k)
        if (info.wrote[k])
          ++nwrote;
      msg.u32(nwrote);
      for (std::size_t k = 0; k < auxsync::kSlots; ++k) {
        if (!info.wrote[k])
          continue;
        msg.u32(static_cast<uint32_t>(k));
        msg.u64(info.old_seq[k]);
      }
      msg.u32(g_stage.entries);
      msg.b += g_stage.payload;
      const int64_t deadline = NowMs() + g_apply_timeout_ms;
          if (SendAll(g_chan.req, msg.b, deadline, false) == MqResult::Ok) {
        std::string reply;
        acked = RecvOne(g_chan.rsp, reply, deadline, false) == MqResult::Ok;
      }
      if (!acked) {
        // The worker is busy or gone: stop queueing behind it for a while.
        // (The counters already tell it that it missed this commit.)
        pg_atomic_write_u64(&shm->skip_ship_until_ms, NowMs() + 3000);
        CloseChannel();
      }
    }
  } catch (...) {
  }
  if (g_stage.inflight_marked && shm) {
    pg_atomic_fetch_sub_u32(&shm->apply_inflight, 1);
    g_stage.inflight_marked = false;
  }
  g_stage.prepared = false;
}

void ResetStage() {
  if (g_stage.inflight_marked && g_shm) {
    pg_atomic_fetch_sub_u32(&g_shm->apply_inflight, 1);
  }
  g_stage = Stage{};
}

// ---- diagnostics ------------------------------------------------------------------------
std::string StatsText() {
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm || pg_atomic_read_u32(&shm->state) != kRunning)
    return "no worker";
  char buf[320];
  snprintf(buf, sizeof(buf),
           "requests=%llu hits=%llu rebuilds=%llu declined=%llu errors=%llu "
           "applies=%llu synced=%llu unsynced=%llu generation=%u pid=%u",
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatRequests]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatHits]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatRebuilds]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatDeclined]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatErrors]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatApplies]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatSynced]),
           (unsigned long long)pg_atomic_read_u64(&shm->stats[kStatUnsynced]),
           pg_atomic_read_u32(&shm->generation),
           pg_atomic_read_u32(&shm->worker_pid));
  return buf;
}

void SetForcedMaintenance(const std::string &action_name) {
  if (!g_shared_state || g_in_server)
    return;
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm)
    return;
  SpinLockAcquire(&shm->text_lock);
  snprintf(shm->forced, sizeof(shm->forced), "%s", action_name.c_str());
  SpinLockRelease(&shm->text_lock);
}

namespace {
std::string ReadText(const char *field, std::size_t size, Shm *shm) {
  char buf[200];
  SpinLockAcquire(&shm->text_lock);
  std::memcpy(buf, field, std::min(size, sizeof(buf)));
  SpinLockRelease(&shm->text_lock);
  buf[std::min(size, sizeof(buf)) - 1] = '\0';
  return buf;
}
} // namespace

std::string LastMaintenanceActionName() {
  if (!g_shared_state || g_in_server)
    return std::string();
  Shm *shm = ShmFor(MyDatabaseId);
  return shm ? ReadText(shm->last_action, sizeof(shm->last_action), shm)
             : std::string();
}

std::string LastMaintenanceDecisionText() {
  if (!g_shared_state || g_in_server)
    return std::string();
  Shm *shm = ShmFor(MyDatabaseId);
  return shm ? ReadText(shm->last_decision, sizeof(shm->last_decision), shm)
             : std::string();
}

bool StopWorker() {
  Shm *shm = ShmFor(MyDatabaseId);
  if (!shm || !WorkerRunning(shm))
    return false;
  kill(static_cast<pid_t>(pg_atomic_read_u32(&shm->worker_pid)), SIGTERM);
  return true;
}

void NoteApplyOutcome(bool synced) {
  if (g_shm)
    Bump(g_shm, synced ? kStatSynced : kStatUnsynced);
}

} // namespace accelerator_codegen::aux_state::engine::postgres::aux_server

// =================================================================================
// Worker entry point
// =================================================================================
namespace as = accelerator_codegen::aux_state::engine::postgres::aux_server;

extern "C" {

void accel_aux_server_main(Datum main_arg) {
  using namespace accelerator_codegen::aux_state::engine::postgres::aux_server;
  const Oid dboid = DatumGetObjectId(main_arg);
  as::g_in_server = true;
  pqsignal(SIGTERM, die);
  pqsignal(SIGHUP, SignalHandlerForConfigReload);
  BackgroundWorkerUnblockSignals();

  as::Shm *shm = as::ShmFor(dboid);
  before_shmem_exit(as::WorkerExit, 0);
  BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);

  SetConfigOption("lock_timeout", "2000", PGC_SUSET, PGC_S_OVERRIDE);
  SetConfigOption("statement_timeout", "0", PGC_SUSET, PGC_S_OVERRIDE);
  SetConfigOption("application_name", "accel aux server", PGC_SUSET,
                  PGC_S_OVERRIDE);

  // A fresh worker has no state: the capture catalog starts empty.
  SpinLockAcquire(&shm->catalog_lock);
  shm->nentries = 0;
  pg_atomic_fetch_add_u32(&shm->catalog_version, 1);
  SpinLockRelease(&shm->catalog_lock);
  pg_atomic_write_u32(&shm->worker_pid, static_cast<uint32>(MyProcPid));
  pg_atomic_write_u32(&shm->worker_procno, static_cast<uint32>(MyProcNumber));
  pg_atomic_write_u32(&shm->busy, 0);
  pg_atomic_write_u32(&shm->apply_inflight, 0);
  pg_atomic_fetch_add_u32(&shm->generation, 1);
  for (int i = 0; i < kStatCount; ++i) // the counters describe this worker
    pg_atomic_write_u64(&shm->stats[i], 0);
  pg_memory_barrier();
  pg_atomic_write_u32(&shm->state, as::kRunning);
  elog(LOG, "accel aux server started for database %u", (unsigned)dboid);

  struct Pending {
    int slot;
    std::string msg;
  };
  std::vector<Pending> deferred;
  int64_t last_activity = as::NowMs();

  for (;;) {
    CHECK_FOR_INTERRUPTS();
    ResetLatch(MyLatch);
    if (ConfigReloadPending) {
      ConfigReloadPending = false;
      ProcessConfigFile(PGC_SIGHUP);
    }
    as::PollSlots(shm);

    for (int i = 0; i < as::kMaxClients; ++i) {
      as::WChan &c = as::g_wchans[i];
      if (!c.seg)
        continue;
      Size n = 0;
      void *data = nullptr;
      const shm_mq_result r = shm_mq_receive(c.req, &n, &data, true);
      if (r == SHM_MQ_SUCCESS) {
        deferred.push_back(Pending{i, std::string(static_cast<char *>(data), n)});
        last_activity = as::NowMs();
      } else if (r == SHM_MQ_DETACHED) {
        as::DropWChan(shm, i, true);
      }
    }

    bool worked = false;
    // Applies first: a commit that is visible but not yet applied must not be
    // mistaken for a stale state by a query.
    for (std::size_t k = 0; k < deferred.size();) {
      if (deferred[k].msg[0] == 'A') {
        Pending p = std::move(deferred[k]);
        deferred.erase(deferred.begin() + k);
        as::HandleApply(shm, p.slot, p.msg);
        worked = true;
      } else {
        ++k;
      }
    }
    const bool apply_pending = pg_atomic_read_u32(&shm->apply_inflight) > 0 &&
                               as::NowMs() - static_cast<int64_t>(pg_atomic_read_u64(
                                                 &shm->apply_since_ms)) <
                                   as::g_apply_timeout_ms;
    if (!apply_pending && !deferred.empty()) {
      Pending p = std::move(deferred.front());
      deferred.erase(deferred.begin());
      as::HandleQuery(shm, p.slot, p.msg);
      worked = true;
    }
    if (worked) {
      last_activity = as::NowMs();
      continue;
    }
    if (as::g_idle_exit_s > 0 &&
        as::NowMs() - last_activity > 1000LL * as::g_idle_exit_s)
      proc_exit(0);
    (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                    apply_pending || !deferred.empty() ? 1 : 100,
                    PG_WAIT_EXTENSION);
  }
}

PG_FUNCTION_INFO_V1(accel_postgres_shared_stats);
Datum accel_postgres_shared_stats(PG_FUNCTION_ARGS) {
  const std::string text = as::StatsText();
  PG_RETURN_TEXT_P(cstring_to_text(text.c_str()));
}

PG_FUNCTION_INFO_V1(accel_postgres_stop_shared_worker);
Datum accel_postgres_stop_shared_worker(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(as::StopWorker());
}

} // extern "C"

#endif
