// The generic host kit for Verus accelerators (see verus_kit.hpp).
#include "substrate/verus/kit/verus_kit.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>

namespace accelerator_codegen::verus_kit {


namespace {

using aux_state::VerusAuxStore;

// ---- registry ----
std::mutex &RegistryMutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, const Glue *> &Registry() {
  static std::map<std::string, const Glue *> r;
  return r;
}

const core::AuxValue &Cell(const core::AuxRow &row, std::size_t i) {
  if (i >= row.values.size() || row.values[i].is_null)
    throw Declined("NULL or missing cell");
  return row.values[i];
}

int64_t Pow10(int n) {
  int64_t p = 1;
  for (int k = 0; k < n; ++k)
    p *= 10;
  return p;
}

// value * 10^from as value * 10^to, exactly (or Declined).
int64_t Rescale(int64_t v, int from, int to) {
  if (from == to)
    return v;
  if (to > from) {
    const int64_t f = Pow10(to - from);
    if (v > std::numeric_limits<int64_t>::max() / f || v < std::numeric_limits<int64_t>::min() / f)
      throw Declined("decimal out of range");
    return v * f;
  }
  const int64_t f = Pow10(from - to);
  if (v % f != 0)
    throw Declined("decimal has more digits than its column type");
  return v / f;
}

// ---- payloads ----
// A kernel state owned by the host store, or a reading of a Verus store (what its
// verified `answer` runs on).
struct KitImage {
  KitImage(const Glue &g, void *s, bool shared_p, void (*release_p)(const void *) = nullptr)
      : glue(g), state(s), shared(shared_p), release(release_p) {}
  ~KitImage() {
    if (!state)
      return;
    if (shared && release)
      release(state);
    else if (shared)
      glue.store.reading_free(state);
    else
      glue.free_state(state);
  }
  KitImage(const KitImage &) = delete;
  KitImage &operator=(const KitImage &) = delete;
  const Glue &glue;
  mutable void *state;
  bool shared;
  void (*release)(const void *);
};

// The next version's slot of one accelerator, staged on the newest version by the
// kernel (consumed by the Verus store).  `base`: the world base it was staged on.
struct KitStaged {
  KitStaged(const Glue &g, void *p, std::shared_ptr<void> base_p = {}) : glue(g), staged(p), base(std::move(base_p)) {}
  ~KitStaged() {
    if (staged)
      glue.store.staged_free(staged);
  }
  KitStaged(const KitStaged &) = delete;
  KitStaged &operator=(const KitStaged &) = delete;
  void *Take() const {
    void *p = staged;
    staged = nullptr;
    return p;
  }
  const Glue &glue;
  mutable void *staged;
  std::shared_ptr<void> base;
};

// What a Verus store gives the module to stage the next slot: apply (null: the slot is
// invalid, rebuild) or build (a Declined scan stages an invalid slot).  The glue takes
// the head after its scans, so the build is staged on the newest version at that point.
class Staging {
public:
  virtual ~Staging() = default;
  virtual const Glue &GlueOf() const = 0;
  virtual std::shared_ptr<const KitStaged> StageApply(const Changes &, Host &, int32_t *err) const = 0;
  virtual std::shared_ptr<const KitStaged> StageBuild(Host &) const = 0;
};

// ---- build by apply ----
// A build whose memory does not scale with the tables: the kernel's build on empty scans, then
// every table's rows in position chunks as insert deltas through the kernel's apply.  The chunk
// ending at position `hi` is applied on a database in which each table holds only its rows
// below `hi`, so the rescans (PrefixHost) agree with the apply's contract.
using Clock = std::chrono::steady_clock;

// Reads nothing: the kernel's build on empty scans.
class EmptyHost final : public Host {
public:
  void Query(const std::string &, const RowFn &) override {}
  void QueryParams(const std::string &, const std::vector<core::AuxValue> &, const RowFn &) override {}
  std::vector<int32_t> Attnums(const std::string &, const std::vector<std::string> &) override { return {}; }
};

// Every query runs behind `prefix` (a WITH clause).
class PrefixHost final : public Host {
public:
  PrefixHost(Host &inner_p, std::string prefix_p) : inner(inner_p), prefix(std::move(prefix_p)) {}
  void Query(const std::string &sql, const RowFn &row) override {
    const auto start = Clock::now();
    inner.Query(prefix + sql, [&](const core::AuxRow &r) { ++rows; row(r); });
    millis += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  }
  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params, const RowFn &row) override {
    inner.QueryParams(prefix + sql, params, row);
  }
  bool QueryBigintList(const std::string &sql, const std::vector<int64_t> &list, const RowFn &row) override {
    const auto start = Clock::now();
    const bool ok = inner.QueryBigintList(prefix + sql, list, [&](const core::AuxRow &r) { ++rows; row(r); });
    millis += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return ok;
  }
  std::vector<int32_t> Attnums(const std::string &t, const std::vector<std::string> &c) override { return inner.Attnums(t, c); }
  std::string Quote(const std::string &identifier) const override { return inner.Quote(identifier); }
  std::string QuoteText(const std::string &text) const override { return inner.QuoteText(text); }
  Host &inner;
  std::string prefix;
  uint64_t rows = 0;  // rescanned
  double millis = 0;  // spent in rescans
};

// Rows per chunk of a build by apply (ACCEL_VERUS_BUILD_CHUNK_ROWS; unset or 0: whole scans).
uint64_t BuildChunkRows() {
  const char *env = std::getenv("ACCEL_VERUS_BUILD_CHUNK_ROWS");
  return env ? std::strtoull(env, nullptr, 10) : 0;
}

struct ChunkedBuild {
  bool used = false;      // false: the tables fit one chunk (or the engine has no positions): build from whole scans
  void *state = nullptr;  // used and null: the kernel declined
};

ChunkedBuild BuildByApply(const Glue &glue, Host &host) {
  const uint64_t chunk = BuildChunkRows();
  if (chunk == 0 || !glue.apply || !glue.build || !glue.free_state)
    return {};
  std::vector<std::string> names;
  std::vector<uint64_t> ends;
  uint64_t longest = 0;
  for (const auto &t : glue.tables) {
    const auto end = host.RowEnd(t.name);
    if (!end)
      return {};
    names.emplace_back(t.name);
    ends.push_back(*end);
    longest = std::max(longest, *end);
  }
  if (longest <= chunk)
    return {};
  const bool trace = std::getenv("ACCEL_VERUS_BUILD_TRACE") != nullptr;
  EmptyHost empty;
  void *state = glue.build(empty);
  try {
    for (uint64_t lo = 0; state && lo < longest; lo += chunk) {
      const uint64_t hi = lo + chunk;
      Changes changes(glue.tables.size());
      const auto scanning = Clock::now();
      for (std::size_t t = 0; t < glue.tables.size(); ++t) {
        if (lo >= ends[t])
          continue;
        std::vector<std::string> columns(glue.tables[t].columns.begin(), glue.tables[t].columns.end());
        host.Query(host.RowRangeSql(names[t], columns, lo, hi),
                   [&](const core::AuxRow &r) { changes[t].push_back(Change{r, +1}); });
      }
      PrefixHost loaded(host, host.RowPrefixSql(names, hi));
      int32_t err = 0;
      const auto applying = Clock::now();
      void *next = glue.apply(state, changes, loaded, &err);
      glue.free_state(state);
      state = next;
      if (trace)
        std::fprintf(stderr, "build by apply %s: rows [%llu, %llu) scan %.0f ms, apply %.0f ms of which rescan %.0f ms (%llu rows)\n", glue.id,
                     static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi),
                     std::chrono::duration<double, std::milli>(applying - scanning).count(),
                     std::chrono::duration<double, std::milli>(Clock::now() - applying).count(), loaded.millis,
                     static_cast<unsigned long long>(loaded.rows));
    }
  } catch (...) {
    if (state)
      glue.free_state(state);
    throw;
  }
  return {true, state};
}

// A build staged through `glue`: `make_head` returns the newest version's head (or throws),
// `free_head` releases it.
template <class MakeHead, class FreeHead>
void *StageBuildOn(const Glue &glue, Host &host, MakeHead make_head, FreeHead free_head) {
  void *head = nullptr;
  const HeadFn head_of = [&]() -> const void * {
    head = make_head();
    return head;
  };
  void *next = nullptr;
  try {
    try {
      const ChunkedBuild chunked = glue.store.stage_adopt ? BuildByApply(glue, host) : ChunkedBuild{};
      if (!chunked.used)
        next = glue.prepare_build(head_of, host);
      else if (chunked.state) {
        try {
          next = glue.store.stage_adopt(head_of(), chunked.state);
        } catch (...) {
          glue.free_state(chunked.state);
          throw;
        }
      } else
        throw Declined("build by apply declined");
    } catch (const Declined &) {
      if (!head)
        head_of();
      next = glue.store.stage_invalid(head);
    }
  } catch (...) {
    if (head)
      free_head(head);
    throw;
  }
  free_head(head);
  return next;
}

constexpr core::PartitionId kState = 0;

// Why this thread's last Verus-store read found no state (see ReadAwaitingPublish's status).
thread_local int32_t t_read_status = 0;

void NoteMiss() {
  static const char *const names[] = {"store:no_state", "store:invalid_slot", "store:pending", "store:not_visible"};
  core::RecordAuxFallbackCause(names[t_read_status >= 1 && t_read_status <= 3 ? t_read_status : 0]);
}

// The kernel (or the host's conversion of the parameters) declined an answer from a valid state.
void NoteDecline() {
  const auto arg = LastArgFailure();
  core::RecordAuxFallbackCause(arg.what.empty()      ? "kit:kernel_none"
                               : arg.outside_type ? "kit:param_outside_type"
                                                  : "kit:host_conversion");
}

// Reads the Verus store's version at `at` through `read(at, tries, &status)`.
// A reader of a pending commit waits for the publish, or, with a bounded wait,
// polls until it. Status: 0 state (returned), 1 invalid, 2 pending, 3 not visible.
template <class Read>
const void *ReadAwaitingPublish(uint64_t pending_wait_micros, core::Version at, int32_t &status,
                                const Read &read) {
  // The verified store's own "newest" is 0 (its engines' epochs are positive).
  if (at == core::kNewestVersion || Fault() == "ignore_snapshot")
    at = 0;
  if (pending_wait_micros == core::kWaitForPublish)
    return read(at, std::numeric_limits<uint64_t>::max(), &status);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(pending_wait_micros);
  const void *state = nullptr;
  do {
    state = read(at, 64, &status);
  } while (status == 2 && std::chrono::steady_clock::now() < deadline);
  return state;
}

// ---- the Verus store ----
class KitStore final : public VerusAuxStore, public Staging {
public:
  KitStore(const Glue &g, EpochProvider epoch_p, FloorProvider floor_p)
      : glue(g), epoch(std::move(epoch_p)), floor(std::move(floor_p)),
        pending_wait_micros(core::PendingWaitMicrosFromEnv()), store(g.store.new_store()) {}
  ~KitStore() override { glue.store.free_store(store); }

  const void *Handle() const { return store; }
  const Glue &GlueOf() const override { return glue; }
  bool WaitsForRebuild() const override { return true; }

  std::shared_ptr<const KitStaged> StageApply(const Changes &changes, Host &host, int32_t *err) const override {
    void *base = glue.store.newest_base(store);
    if (!base)
      return nullptr;
    void *next = nullptr;
    try {
      next = glue.prepare_apply(base, changes, host, err);
    } catch (...) {
      glue.store.base_free(base);
      throw;
    }
    glue.store.base_free(base);
    return next ? std::make_shared<const KitStaged>(glue, next) : nullptr;
  }

  std::shared_ptr<const KitStaged> StageBuild(Host &host) const override {
    void *next = StageBuildOn(
        glue, host,
        [&] {
          void *head = glue.store.newest_head(store);
          if (!head)
            throw std::runtime_error(std::string(glue.id) + ": no version can follow the newest");
          return head;
        },
        [&](void *head) { glue.store.head_free(head); });
    return std::make_shared<const KitStaged>(glue, next);
  }

  core::StateSnapshot Read(const core::VisibilityToken &visibility) const override {
    core::StateSnapshot snapshot;
    snapshot.accelerator = glue.id;
    snapshot.visibility = visibility;
    int32_t status = 0;
    if (auto image = Resolve(visibility.mutation_epoch, status))
      snapshot.partitions.push_back(core::StateImage::Make(glue.id, kState, 0, std::move(image)));
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }

  core::StateSnapshot ReadPartitions(const core::VisibilityToken &visibility,
                                     const std::vector<core::PartitionId> &partitions) const override {
    if (std::find(partitions.begin(), partitions.end(), kState) != partitions.end())
      return Read(visibility);
    core::StateSnapshot snapshot;
    snapshot.visibility = visibility;
    return snapshot;
  }

  core::StateSnapshot ReadHitPartitions(const core::VisibilityToken &visibility, const core::QueryKey &key,
                                        const std::vector<core::PartitionId> &partitions) const override {
    core::StateSnapshot snapshot;
    snapshot.visibility = visibility;
    if (key.accelerator != glue.id || partitions.empty())
      return snapshot;
    for (auto p : partitions)
      if (p != kState)
        return snapshot;
    return Read(visibility);
  }

  core::ValidityResult CheckValidity(const core::QueryKey &key,
                                     const core::VisibilityToken &visibility) const override {
    core::ValidityResult result;
    result.code = core::ValidityCode::Missing;
    if (key.accelerator != glue.id || (!key.value.empty() && key.value != "0"))
      return result;
    int32_t status = 0;
    Resolve(visibility.mutation_epoch, status);
    result.code = status == 0   ? core::ValidityCode::Valid
                  : status == 1 ? core::ValidityCode::Invalidated
                                : core::ValidityCode::NotVisible;
    return result;
  }

  void Invalidate(const core::InvalidationPlan &plan) override {
    if (!plan.IsEmpty())
      glue.store.invalidate_all(store);
  }

  bool TryPublish(const core::StagedMaintenanceOutcome &staged, core::Version) override {
    InstallUpdates(staged.invalidations, staged.updates, CurrentEpoch(), false);
    return true;
  }

  void BeginCommit(const core::AcceleratorId &accelerator, const core::InvalidationPlan &) override {
    if (!glue.store.begin(store, CurrentEpoch()))
      throw std::logic_error("concurrent commits on aux state of " + accelerator);
  }

  void FinishCommit(const core::AcceleratorId &, const core::InvalidationPlan &invalidate,
                    const std::vector<core::StateImage> &updates, core::Version) override {
    InstallUpdates(invalidate, updates, CurrentEpoch(), true);
  }

  void AbortCommit(const core::AcceleratorId &) override { glue.store.abort(store); }

  void Install(std::vector<core::StateImage> images, const core::AcceleratorId &,
               Epoch from_epoch) override {
    InstallUpdates(core::InvalidationPlan{}, images, from_epoch, false);
  }

  std::size_t ValidPartitionCount() const override {
    int32_t status = 0;
    const void *s = glue.store.read(store, 0, 0, &status);
    if (s)
      glue.store.reading_free(s);
    return status == 0 ? 1 : 0;
  }
  std::size_t TotalPartitionCount() const override { return 1; }
  std::size_t TotalVersionCount() const override { return glue.store.version_count(store); }
  std::size_t HeapBytes() const override { return glue.store.heap_bytes(); }
  std::size_t HeapPeakBytes() const override { return glue.store.heap_peak_bytes(); }
  void ResetHeapPeak() const override { glue.store.heap_reset_peak(); }
  std::size_t StaleInstalls() const override { return stale_installs; }

private:
  Epoch CurrentEpoch() const { return epoch ? epoch() : 0; }
  Epoch CurrentFloor() const { return floor ? floor() : std::numeric_limits<Epoch>::max(); }

  // status: see ReadAwaitingPublish.
  std::shared_ptr<const KitImage> Resolve(core::Version at, int32_t &status) const {
    const core::StoreResolveTimer timer;
    const void *s = ReadAwaitingPublish(pending_wait_micros, at, status,
                                        [&](uint64_t epoch, uint64_t tries, int32_t *st) {
                                          return glue.store.read(store, epoch, tries, st);
                                        });
    t_read_status = s ? 0 : status;
    if (!s)
      return nullptr;
    return std::make_shared<const KitImage>(glue, const_cast<void *>(s), /*shared=*/true);
  }

  void InstallUpdates(const core::InvalidationPlan &invalidate, const std::vector<core::StateImage> &updates,
                      Epoch from, bool end_commit) {
    void *next = nullptr;
    bool invalid = !invalidate.IsEmpty();
    for (const auto &image : updates) {
      if (!image.payload) {
        invalid = true;
        continue;
      }
      if (void *taken = image.As<KitStaged>()->Take()) {
        if (next)
          glue.store.staged_free(next);
        next = taken;
      }
    }
    if (!next && invalid) {
      if (void *head = glue.store.newest_head(store)) {
        next = glue.store.stage_invalid(head);
        glue.store.head_free(head);
      }
    }
    if (!next) {
      if (end_commit)
        glue.store.abort(store);
      return;
    }
    if (glue.store.install(store, next, from, CurrentFloor(), end_commit ? 1 : 0) != 0)
      ++stale_installs, core::RecordAuxFallbackCause("event:install_refused");
  }

  const Glue &glue;
  EpochProvider epoch;
  FloorProvider floor;
  uint64_t pending_wait_micros;
  void *store;
  std::size_t stale_installs = 0;
};

// ---- the world store (ACCEL_AUX_STORE=verus_global) ----
// One verified store for every accelerator of a benchmark (world_glue.cpp).  A commit's
// accelerators each stage their new slot on the same base (the newest version's world,
// `Base()`); the first to begin marks the commit (its engine epoch) in flight, each finish
// puts its slot in one batch made from that base, and the end of the commit installs the
// batch as ONE version (or, if any aborted, drops it).  Commits of disjoint accelerators
// may publish at once: one is assembled at a time and the others wait at `Begin`.  The
// batch takes a slot only if it was staged on its base and put for its commit's epoch,
// and the store installs it only as the commit marked in flight (verified, world.rs,
// store.rs); the assembly here is host code, the store, the batch's world and every
// reading are verified.
std::vector<const World *> &Worlds() {
  static std::vector<const World *> worlds;
  return worlds;
}

class WorldShared {
public:
  explicit WorldShared(const World &w) : world(w), store(w.new_store()) {}
  ~WorldShared() { world.free_store(store); }

  static WorldShared &Of(const World &w) {
    static std::mutex mutex;
    static std::map<const World *, std::unique_ptr<WorldShared>> shared;
    std::lock_guard<std::mutex> lock(mutex);
    auto &slot = shared[&w];
    if (!slot)
      slot = std::make_unique<WorldShared>(w);
    return *slot;
  }

  // The newest version as a base to stage on, shared by the commit's accelerators while
  // the newest version and the world's slots are unchanged (null: no version can follow).
  std::shared_ptr<void> Base() {
    std::lock_guard<std::mutex> lock(mutex);
    return BaseLocked();
  }

  // The commit at engine epoch `from` takes part: opens it (waiting for another commit
  // in flight to end) or joins the open one.
  void Begin(VerusAuxStore::Epoch from) {
    std::unique_lock<std::mutex> lock(mutex);
    ended.wait(lock, [&] { return !open || open_from == from; });
    if (!open) {
      if (!world.begin(store, from))
        throw std::logic_error(std::string("commit already in flight on the Verus world ") + world.bench);
      open = true;
      open_from = from;
    }
  }

  // set: the accelerator's slot changes (the staged slot, or null for invalid);
  // otherwise it only took part.
  void Finish(const WorldKitOps &kit, bool set, const KitStaged *staged, VerusAuxStore::Epoch from,
              VerusAuxStore::Epoch floor) {
    std::lock_guard<std::mutex> lock(mutex);
    open_floor = floor;
    if (set)
      Put(kit, staged, from);
  }

  void Abort() {
    std::lock_guard<std::mutex> lock(mutex);
    aborted = true;
  }

  // Every accelerator of the commit at `from` has finished: install its version.
  void End(VerusAuxStore::Epoch from) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!open || open_from != from)
      return;
    if (!aborted && batch && world.install(store, batch, open_floor, 1) != 0) {
      NoteStale();
      world.abort(store); // a refusal may leave the commit marked
    } else if (aborted || !batch) {
      if (batch)
        world.batch_free(batch);
      world.abort(store);
    }
    Reset();
  }

  // A version outside a commit (a build): this accelerator's slot only.  `from` is the
  // epoch the build's scans read at.  A refused install (a commit in flight, or a newer
  // version) drops the build, counted as stale.
  void InstallNow(const WorldKitOps &kit, const KitStaged *staged, VerusAuxStore::Epoch from,
                  VerusAuxStore::Epoch floor) {
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<void> gb = staged && staged->base ? staged->base : BaseLocked();
    ++generation;
    void *b = gb ? world.batch_new(gb.get(), from) : nullptr;
    if (!b)
      return;
    if (staged)
      NotePut(kit.put(b, staged->Take(), from));
    else
      kit.put_invalid(b);
    if (world.install(store, b, floor, 0) != 0)
      NoteStale();
  }

  // The slot's versions are invalidated: a cached base may carry the old slot.
  void Invalidated() {
    std::lock_guard<std::mutex> lock(mutex);
    ++generation;
  }

  const World &world;
  void *store;
  std::size_t stale = 0;

private:
  void NoteStale() {
    ++stale;
    core::RecordAuxFallbackCause("event:install_refused");
  }
  static void NotePut(int32_t taken) {
    if (!taken)
      core::RecordAuxFallbackCause("event:put_refused");
  }
  std::shared_ptr<void> BaseLocked() {
    const uint64_t newest = world.newest(store);
    if (!base || base_epoch != newest || base_generation != generation) {
      void *gb = world.gbase_new(store);
      if (!gb)
        return nullptr;
      base = std::shared_ptr<void>(gb, [w = &world](void *p) { w->gbase_free(p); });
      base_epoch = newest;
      base_generation = generation;
    }
    return base;
  }

  // The slot staged on the batch's base is put; a slot staged elsewhere, or none, is invalid.
  void Put(const WorldKitOps &kit, const KitStaged *staged, VerusAuxStore::Epoch from) {
    if (!batch) {
      std::shared_ptr<void> gb = staged && staged->base ? staged->base : BaseLocked();
      if (gb)
        batch = world.batch_new(gb.get(), from);
    }
    if (!batch)
      return;
    if (staged)
      NotePut(kit.put(batch, staged->Take(), from));
    else
      kit.put_invalid(batch);
  }

  void Reset() {
    batch = nullptr;
    open = aborted = false;
    ++generation;
    ended.notify_all();
  }

  // mutex guards the commit assembly below (shared by every accelerator of the
  // world): a leaf lock, taken inside the accelerators' commit locks.
  std::mutex mutex;
  std::condition_variable ended;
  bool open = false, aborted = false; // a commit (at open_from) is being assembled
  VerusAuxStore::Epoch open_from = 0, open_floor = 0;
  void *batch = nullptr;
  std::shared_ptr<void> base;
  uint64_t base_epoch = 0, base_generation = 0, generation = 1;
};

class WorldKitStore final : public VerusAuxStore, public Staging {
public:
  WorldKitStore(const Glue &g, const WorldKitOps &k, WorldShared &w, EpochProvider epoch_p, FloorProvider floor_p)
      : glue(g), kit(k), world(w), epoch(std::move(epoch_p)), floor(std::move(floor_p)),
        pending_wait_micros(core::PendingWaitMicrosFromEnv()) {}

  const Glue &GlueOf() const override { return glue; }
  bool WaitsForRebuild() const override { return true; }

  std::shared_ptr<const KitStaged> StageApply(const Changes &changes, Host &host, int32_t *err) const override {
    std::shared_ptr<void> gb = world.Base();
    void *base = gb ? kit.base(gb.get()) : nullptr;
    if (!base)
      return nullptr;
    void *next = nullptr;
    try {
      next = glue.prepare_apply(base, changes, host, err);
    } catch (...) {
      glue.store.base_free(base);
      throw;
    }
    glue.store.base_free(base);
    return next ? std::make_shared<const KitStaged>(glue, next, std::move(gb)) : nullptr;
  }

  std::shared_ptr<const KitStaged> StageBuild(Host &host) const override {
    std::shared_ptr<void> gb;
    void *next = StageBuildOn(
        glue, host,
        [&] {
          gb = world.Base();
          if (!gb)
            throw std::runtime_error(std::string(glue.id) + ": no version can follow the newest");
          return kit.head(gb.get());
        },
        [&](void *head) { glue.store.head_free(head); });
    return std::make_shared<const KitStaged>(glue, next, std::move(gb));
  }

  core::StateSnapshot Read(const core::VisibilityToken &visibility) const override {
    core::StateSnapshot snapshot;
    snapshot.accelerator = glue.id;
    snapshot.visibility = visibility;
    int32_t status = 0;
    if (auto image = Resolve(visibility.mutation_epoch, status))
      snapshot.partitions.push_back(core::StateImage::Make(glue.id, kState, 0, std::move(image)));
    snapshot.available = !snapshot.partitions.empty();
    return snapshot;
  }

  core::StateSnapshot ReadPartitions(const core::VisibilityToken &visibility,
                                     const std::vector<core::PartitionId> &partitions) const override {
    if (std::find(partitions.begin(), partitions.end(), kState) != partitions.end())
      return Read(visibility);
    core::StateSnapshot snapshot;
    snapshot.visibility = visibility;
    return snapshot;
  }

  core::StateSnapshot ReadHitPartitions(const core::VisibilityToken &visibility, const core::QueryKey &key,
                                        const std::vector<core::PartitionId> &partitions) const override {
    core::StateSnapshot snapshot;
    snapshot.visibility = visibility;
    if (key.accelerator != glue.id || partitions.empty())
      return snapshot;
    for (auto p : partitions)
      if (p != kState)
        return snapshot;
    return Read(visibility);
  }

  core::ValidityResult CheckValidity(const core::QueryKey &key,
                                     const core::VisibilityToken &visibility) const override {
    core::ValidityResult result;
    result.code = core::ValidityCode::Missing;
    if (key.accelerator != glue.id || (!key.value.empty() && key.value != "0"))
      return result;
    int32_t status = 0;
    Resolve(visibility.mutation_epoch, status);
    result.code = status == 0   ? core::ValidityCode::Valid
                  : status == 1 ? core::ValidityCode::Invalidated
                                : core::ValidityCode::NotVisible;
    return result;
  }

  void Invalidate(const core::InvalidationPlan &plan) override {
    if (plan.IsEmpty())
      return;
    kit.invalidate(world.store);
    world.Invalidated();
  }

  bool TryPublish(const core::StagedMaintenanceOutcome &staged, core::Version) override {
    bool set = false;
    const auto slot = Slot(staged.invalidations, staged.updates, set);
    if (set)
      world.InstallNow(kit, slot.get(), CurrentEpoch(), CurrentFloor());
    return true;
  }

  void BeginCommit(const core::AcceleratorId &, const core::InvalidationPlan &) override {
    world.Begin(CurrentEpoch());
  }

  void FinishCommit(const core::AcceleratorId &, const core::InvalidationPlan &invalidate,
                    const std::vector<core::StateImage> &updates, core::Version) override {
    bool set = false;
    const auto slot = Slot(invalidate, updates, set);
    world.Finish(kit, set, slot.get(), CurrentEpoch(), CurrentFloor());
  }

  void AbortCommit(const core::AcceleratorId &) override { world.Abort(); }
  void EndCommit(uint64_t epoch) override { world.End(epoch); }

  void Install(std::vector<core::StateImage> images, const core::AcceleratorId &, Epoch from_epoch) override {
    bool set = false;
    const auto slot = Slot(core::InvalidationPlan{}, images, set);
    if (set)
      world.InstallNow(kit, slot.get(), from_epoch, CurrentFloor());
  }

  std::size_t ValidPartitionCount() const override {
    int32_t status = 0;
    const void *s = kit.read(world.store, 0, 0, &status);
    if (s)
      kit.release(s);
    return status == 0 ? 1 : 0;
  }
  std::size_t TotalPartitionCount() const override { return 1; }
  std::size_t TotalVersionCount() const override { return world.world.version_count(world.store); }
  std::size_t HeapBytes() const override { return world.world.heap_bytes(); }
  std::size_t HeapPeakBytes() const override { return world.world.heap_peak_bytes(); }
  void ResetHeapPeak() const override { world.world.heap_reset_peak(); }
  std::size_t StaleInstalls() const override { return world.stale; }

private:
  Epoch CurrentEpoch() const { return epoch ? epoch() : 0; }
  Epoch CurrentFloor() const { return floor ? floor() : std::numeric_limits<Epoch>::max(); }

  // The slot's change: the last update's staged slot, or none (invalid); `set` false if
  // the commit leaves the slot as it is.
  static std::shared_ptr<const KitStaged> Slot(const core::InvalidationPlan &invalidate,
                                               const std::vector<core::StateImage> &updates, bool &set) {
    std::shared_ptr<const KitStaged> slot;
    set = !invalidate.IsEmpty();
    for (const auto &image : updates) {
      set = true;
      if (image.payload)
        slot = std::shared_ptr<const KitStaged>(image.payload, image.As<KitStaged>());
    }
    return slot;
  }

  // status: see ReadAwaitingPublish.
  std::shared_ptr<const KitImage> Resolve(core::Version at, int32_t &status) const {
    const core::StoreResolveTimer timer;
    const void *s = ReadAwaitingPublish(pending_wait_micros, at, status,
                                        [&](uint64_t epoch, uint64_t tries, int32_t *st) {
                                          return kit.read(world.store, epoch, tries, st);
                                        });
    t_read_status = s ? 0 : status;
    if (!s)
      return nullptr;
    return std::make_shared<const KitImage>(glue, const_cast<void *>(s), /*shared=*/true, kit.release);
  }

  const Glue &glue;
  const WorldKitOps &kit;
  WorldShared &world;
  EpochProvider epoch;
  FloorProvider floor;
  uint64_t pending_wait_micros;
};

// ---- the module ----
class KitBuffer final : public core::AuxTxnBuffer {
public:
  explicit KitBuffer(std::size_t tables) : changes(tables) {}
  bool HasPendingChanges() const override { return pending; }
  void Clear() override {
    pending = false;
    requires_rebuild = false;
    for (auto &t : changes)
      t.clear();
  }
  void MarkRebuild() {
    Clear();
    pending = true;
    requires_rebuild = true;
  }
  Changes changes;
  bool pending = false;
  bool requires_rebuild = false;
};

// Rescans return nothing (fault no_rescan): every other query passes through.
class NoRescanHost final : public Host {
public:
  explicit NoRescanHost(Host &inner_p) : inner(inner_p) {}
  void Query(const std::string &, const RowFn &) override {}
  void QueryParams(const std::string &sql, const std::vector<core::AuxValue> &params, const RowFn &row) override {
    inner.QueryParams(sql, params, row);
  }
  std::vector<int32_t> Attnums(const std::string &t, const std::vector<std::string> &c) override {
    return inner.Attnums(t, c);
  }
  Host &inner;
};

void Rebuild(core::StagedMaintenanceOutcome &outcome) {
  core::RecordAuxFallbackCause("event:rebuild_requested");
  outcome.action = core::MaintenanceAction::RebuildAll;
  outcome.invalidations = core::AllPartitionsPlan();
  outcome.pending_rebuild = outcome.invalidations;
}

class KitModule final : public core::AuxQueryModule {
public:
  KitModule(const Glue &g, Host &h) : glue(g), host(h) {}

  core::AcceleratorId Id() const override { return glue.id; }

  std::vector<core::TableDependency> Dependencies() const override {
    std::lock_guard<std::mutex> lock(deps_mutex);
    if (deps.empty()) {
      for (std::size_t i = 0; i < glue.tables.size(); ++i) {
        const auto &t = glue.tables[i];
        std::vector<std::string> cols(t.columns.begin(), t.columns.end());
        core::TableDependency d;
        d.table = TableIdOf(i);
        for (std::size_t c = 0; c < cols.size(); ++c)
          d.watched_columns.push_back(static_cast<core::ColumnId>(c));
        d.mutation_attnums = host.Attnums(t.name, cols);
        deps.push_back(std::move(d));
      }
    }
    return deps;
  }

  std::unique_ptr<core::AuxTxnBuffer> CreateTxnBuffer() const override {
    return std::make_unique<KitBuffer>(glue.tables.size());
  }

  void Build(core::AuxEngineServices &services, core::StateBuilder &builder) override {
    if (const auto *staging = StagingStore(services)) {
      builder.Write(core::StateImage::Make(glue.id, kState, 0, staging->StageBuild(host)));
      builder.Finish();
      return;
    }
    void *state = nullptr;
    try {
      const ChunkedBuild chunked = BuildByApply(glue, host);
      state = chunked.used ? chunked.state : glue.build(host);
    } catch (const Declined &) {
      state = nullptr;
    }
    builder.Write(core::StateImage::Make(glue.id, kState, 0, std::make_shared<const KitImage>(glue, state, false)));
    builder.Finish();
  }

  core::InvalidationPlan InterpretMutation(const core::MutationBatch &mutation) const override {
    core::InvalidationPlan plan;
    if (TableIndex(mutation.table) < glue.tables.size())
      plan.scope = aux_state::InvalidationScope::PartitionSet;
    return plan;
  }

  void AccumulateMutation(const core::MutationBatch &mutation, core::AuxEngineServices &,
                          core::AuxTxnBuffer &base_buffer) override {
    auto &buffer = dynamic_cast<KitBuffer &>(base_buffer);
    const std::size_t t = TableIndex(mutation.table);
    if (buffer.requires_rebuild || t >= glue.tables.size())
      return;
    const std::size_t width = glue.tables[t].columns.size();
    auto add = [&](const core::AuxRow &row, int64_t sign) {
      if (row.values.size() != width)
        return false;
      buffer.changes[t].push_back(Change{row, sign});
      buffer.pending = true;
      return true;
    };
    const bool del = mutation.kind == core::MutationKind::Delete || mutation.kind == core::MutationKind::Update;
    const bool ins = mutation.kind == core::MutationKind::Insert || mutation.kind == core::MutationKind::Update;
    if (del)
      for (const auto &row : mutation.old_rows)
        if (!add(row, -1))
          return buffer.MarkRebuild();
    if (ins)
      for (const auto &row : mutation.new_rows)
        if (!add(row, +1))
          return buffer.MarkRebuild();
  }

  core::StagedMaintenanceOutcome StageMaintenance(core::AuxEngineServices &services, const core::InvalidationPlan &,
                                                  core::AuxTxnBuffer &base_buffer) override {
    auto &buffer = dynamic_cast<KitBuffer &>(base_buffer);
    core::StagedMaintenanceOutcome outcome;
    if (buffer.requires_rebuild) {
      core::RecordAuxFallbackCause("event:rebuild_unconvertible_change");
      const auto decision = services.ChooseMaintenance(
          {0, 0, false, false, true, true, false, core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      outcome.action = decision.action == core::MaintenanceAction::ApplyDelta ? core::MaintenanceAction::RebuildAll
                                                                              : decision.action;
      outcome.invalidations = core::AllPartitionsPlan();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
    }
    std::size_t total = 0;
    for (const auto &t : buffer.changes)
      total += t.size();
    if (total == 0)
      return outcome;
    if (std::getenv("ACCEL_VERUS_RECORD_DIR")) {
      // Record mode (glue contract checks): the glue marshals the changes
      // (the C ABI logs them and returns without a kernel); nothing changes.
      int32_t err = 0;
      try {
        glue.apply(nullptr, buffer.changes, host, &err);
      } catch (const Declined &) {
      }
      return outcome;
    }
    if (Fault() == "skip_apply")
      return outcome; // deliberate bug: the state misses the commit
    // A Verus store always applies (the proofs make apply total below the store's history);
    // it rebuilds only when the apply declines or there is no state to apply to.
    if (!StagingStore(services)) {
      const auto decision = services.ChooseMaintenance(
          {static_cast<uint64_t>(total), 1, true, false, true, false, false, core::MaintenanceAction::NoOp,
           services.ExpectedQueriesBeforeNextWrite()});
      if (decision.action == core::MaintenanceAction::RebuildPartition) {
        core::RecordAuxFallbackCause("event:rebuild_cost_model");
        core::FinishRebuildAll(outcome);
        return outcome;
      }
      if (decision.action != core::MaintenanceAction::ApplyDelta) {
        core::RecordAuxFallbackCause("event:rebuild_cost_model");
        outcome.action = decision.action;
        outcome.invalidations = core::AllPartitionsPlan();
        outcome.pending_rebuild = outcome.invalidations;
        return outcome;
      }
    }
    Changes changes = buffer.changes;
    const std::string &fault = Fault();
    if (fault.rfind("drop_delta:", 0) == 0) {
      const std::string table = fault.substr(11);
      for (std::size_t i = 0; i < glue.tables.size(); ++i)
        if (table == glue.tables[i].name)
          changes[i].clear();
    }
    NoRescanHost no_rescan(host);
    Host &h = fault == "no_rescan" ? static_cast<Host &>(no_rescan) : host;
    int32_t err = 0;
    try {
      if (const auto *staging = StagingStore(services)) {
        const auto staged = staging->StageApply(changes, h, &err);
        if (!staged) {
          Rebuild(outcome);
          return outcome;
        }
        outcome.action = core::MaintenanceAction::ApplyDelta;
        outcome.updates.push_back(core::StateImage::Make(glue.id, kState, 0, staged));
        return outcome;
      }
      const auto image = ReadImage(services, services.CurrentVisibility());
      if (!image || !image->state) {
        Rebuild(outcome);
        return outcome;
      }
      void *next = glue.apply(image->state, changes, h, &err);
      if (!next) {
        Rebuild(outcome);
        return outcome;
      }
      outcome.action = core::MaintenanceAction::ApplyDelta;
      outcome.updates.push_back(
          core::StateImage::Make(glue.id, kState, 0, std::make_shared<const KitImage>(glue, next, false)));
    } catch (const Declined &) {
      Rebuild(outcome);
    }
    return outcome;
  }

  // The query's registered calibration writes (plain SQL on its tables, from
  // accelerators/<bench>/q<N>/calibration_writes.hpp): the same for any
  // accelerator of the query.
  std::vector<core::CalibrationWrite> CalibrationWrites() const override {
    return core::CalibrationWritesFor(glue.workload, glue.query_id);
  }

  bool CheckValidity(core::AuxEngineServices &services, const core::QueryArguments &) const override {
    return services.StateStore().CheckValidity({glue.id, {}}, services.CurrentVisibility()).IsValid();
  }

  void Execute(core::AuxEngineServices &services, const core::QueryArguments &arguments,
               core::AuxRowSink &sink) override {
    // decline_execute (a test switch): every read answered by the SQL fallback, which
    // must be as right as the kernel (the integration gate checks it)
    if (Fault() == "decline_execute") {
      core::RecordAuxFallbackCause("kit:injected");
      ExecuteFallback(services, arguments, sink);
      return;
    }
    const auto image = ReadImage(services, services.CurrentVisibility());
    if (!image || !image->state) {
      NoteMiss();
      ExecuteFallback(services, arguments, sink);
      return;
    }
    ClearArgFailure();
    if (glue.execute_into && !image->shared) {
      // streamed: the glue emits (and may reuse) one row at a time
      bool begun = false;
      const RowFn emit = [&](const core::AuxRow &row) {
        if (!begun) {
          sink.Begin(core::OutputSchema{});
          begun = true;
        }
        sink.WriteRow(row);
      };
      if (!glue.execute_into(image->state, arguments, emit)) {
        NoteDecline();
        ExecuteFallback(services, arguments, sink);
        return;
      }
      if (!begun)
        sink.Begin(core::OutputSchema{});
      sink.End();
      return;
    }
    // A reused vector keeps its capacity across reads on this thread.
    thread_local std::vector<core::AuxRow> rows;
    rows.clear();
    if (!(image->shared ? glue.answer : glue.execute)(image->state, arguments, rows)) {
      rows.clear();
      NoteDecline();
      ExecuteFallback(services, arguments, sink);
      return;
    }
    sink.Begin(core::OutputSchema{});
    // Free each row right after it is written (cache-warm) instead of one
    // cold destruction pass over the whole answer afterwards.
    for (auto &row : rows) {
      sink.WriteRow(row);
      std::vector<core::AuxValue>().swap(row.values);
    }
    rows.clear();
    sink.End();
  }

  void ExecuteFallback(core::AuxEngineServices &, const core::QueryArguments &arguments,
                       core::AuxRowSink &sink) override {
    sink.Begin(core::OutputSchema{});
    host.QueryParams(glue.query_sql, arguments.values, [&](const core::AuxRow &row) { sink.WriteRow(row); });
    sink.End();
  }

private:
  std::size_t TableIndex(core::TableId table) const {
    return table >= 1 && table <= glue.tables.size() ? static_cast<std::size_t>(table - 1) : glue.tables.size();
  }

  // The Verus store staging this query's slots (its own, or its view of the world), if any.
  const Staging *StagingStore(core::AuxEngineServices &services) const {
    const auto *staging = dynamic_cast<const Staging *>(&services.StateStore());
    return staging && &staging->GlueOf() == &glue ? staging : nullptr;
  }

  std::shared_ptr<const KitImage> ReadImage(core::AuxEngineServices &services,
                                            const core::VisibilityToken &visibility) const {
    const auto snapshot = services.StateStore().ReadPartitions(visibility, {kState});
    for (const auto &image : snapshot.partitions)
      if (image.partition == kState && image.payload)
        return std::shared_ptr<const KitImage>(image.payload, image.As<KitImage>());
    return nullptr;
  }

  const Glue &glue;
  Host &host;
  mutable std::mutex deps_mutex; // guards the lazily resolved deps
  mutable std::vector<core::TableDependency> deps;
};

} // namespace

// ---- cells ----
int64_t I64(const core::AuxRow &row, std::size_t i, int scale) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<int64_t>(&v.payload))
    return Rescale(*x, 0, scale);
  if (const auto *x = std::get_if<uint64_t>(&v.payload)) {
    if (*x > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      throw Declined("integer out of range");
    return Rescale(static_cast<int64_t>(*x), 0, scale);
  }
  if (const auto *x = std::get_if<core::ScaledDecimal>(&v.payload))
    return Rescale(x->value, x->scale, scale);
  if (const auto *x = std::get_if<core::AuxDate>(&v.payload))
    return Rescale(x->days_since_epoch, 0, scale);
  if (const auto *x = std::get_if<bool>(&v.payload))
    return Rescale(*x ? 1 : 0, 0, scale);
  throw Declined("not an integer or decimal cell");
}

int32_t Date(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<core::AuxDate>(&v.payload))
    return x->days_since_epoch;
  if (const auto *x = std::get_if<int64_t>(&v.payload)) {
    if (*x < std::numeric_limits<int32_t>::min() || *x > std::numeric_limits<int32_t>::max())
      throw Declined("date out of range");
    return static_cast<int32_t>(*x);
  }
  throw Declined("not a date cell");
}

namespace {
// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
void CivilFromDays(int64_t z, int64_t &y, int64_t &m, int64_t &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yoe + era * 400 + (m <= 2);
}
} // namespace

int64_t Ts(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<int64_t>(&v.payload))
    return *x;
  if (const auto *x = std::get_if<core::AuxDate>(&v.payload))
    return static_cast<int64_t>(x->days_since_epoch) * 86400LL * 1000000000LL;
  const auto *s = std::get_if<std::string>(&v.payload);
  if (!s)
    throw Declined("not a timestamp cell");
  // YYYY-MM-DD[ HH:MM:SS[.fffffffff]]
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, n = 0;
  char frac[16] = {0};
  const int got = std::sscanf(s->c_str(), "%d-%d-%d %d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n);
  if (got < 3)
    throw Declined("unparsable timestamp " + *s);
  int64_t ns = 0;
  if (got == 6 && static_cast<std::size_t>(n) < s->size() && (*s)[static_cast<std::size_t>(n)] == '.') {
    std::size_t k = 0;
    for (std::size_t j = static_cast<std::size_t>(n) + 1; j < s->size() && k < 9 && std::isdigit(static_cast<unsigned char>((*s)[j])); ++j)
      frac[k++] = (*s)[j];
    std::string f(frac, k);
    f.append(9 - k, '0');
    ns = std::stoll(f);
  } else if (got == 6 && static_cast<std::size_t>(n) < s->size()) {
    throw Declined("timestamp with a time zone or suffix: " + *s);
  }
  return ((DaysFromCivil(y, mo, d) * 24 + h) * 60 + mi) * 60 * 1000000000LL + se * 1000000000LL + ns;
}

uint8_t Bool(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<bool>(&v.payload))
    return *x ? 1 : 0;
  if (const auto *x = std::get_if<int64_t>(&v.payload))
    return *x != 0 ? 1 : 0;
  throw Declined("not a boolean cell");
}

bool IsNull(const core::AuxRow &row, std::size_t i) { return i >= row.values.size() || row.values[i].is_null; }

const std::string kNoText;

uint64_t F64(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  double d;
  if (const auto *x = std::get_if<double>(&v.payload))
    d = *x;
  else
    throw Declined("not a floating-point cell");
  if (!std::isfinite(d))
    throw Declined("non-finite DOUBLE");
  uint64_t bits;
  std::memcpy(&bits, &d, sizeof bits);
  return bits;
}

const std::string &Text(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<std::string>(&v.payload))
    return *x;
  throw Declined("not a text cell");
}

// ---- arguments ----
namespace {
thread_local ArgFailure g_arg_failure;

// outside_type: the value is not one of the parameter's type (NULL, more digits than its scale,
// beyond its range), so SQL answers it; otherwise the host failed to convert an in-type value.
bool ArgFailed(std::size_t i, const std::string &why, bool outside_type) {
  g_arg_failure = {"argument " + std::to_string(i) + ": " + why, outside_type};
  return false;
}

// A conversion error that says the value does not fit the type (the range and scale checks).
bool OutsideType(const std::exception &e) { return std::string(e.what()).rfind("not a", 0) != 0; }

const core::AuxValue *Arg(const core::QueryArguments &args, std::size_t i) {
  return i < args.values.size() && !args.values[i].is_null ? &args.values[i] : nullptr;
}
} // namespace

void ClearArgFailure() {
  g_arg_failure = {};
}

ArgFailure LastArgFailure() {
  return g_arg_failure;
}

bool ArgIsNull(const core::QueryArguments &args, std::size_t i) {
  return i < args.values.size() && args.values[i].is_null;
}

bool ArgText(const core::QueryArguments &args, std::size_t i, std::string &out) {
  const auto *v = Arg(args, i);
  if (!v)
    return ArgFailed(i, "NULL", true);
  const auto *s = std::get_if<std::string>(&v->payload);
  if (!s)
    return ArgFailed(i, "not a text value", false);
  out = *s;
  return true;
}

bool ArgI64(const core::QueryArguments &args, std::size_t i, int scale, int64_t &out) {
  const auto *v = Arg(args, i);
  if (!v)
    return ArgFailed(i, "NULL", true);
  try {
    if (const auto *d = std::get_if<double>(&v->payload)) {
      const double scaled = *d * static_cast<double>(Pow10(scale));
      if (!std::isfinite(scaled) || std::fabs(scaled - std::nearbyint(scaled)) > 1e-6 ||
          std::fabs(scaled) > 9.0e18)
        return ArgFailed(i, "double value " + std::to_string(*d) + " is not exact at scale " + std::to_string(scale), false);
      out = static_cast<int64_t>(std::llround(scaled));
      return true;
    }
    if (const auto *s = std::get_if<std::string>(&v->payload)) {
      // An exact decimal literal: digits [. digits].
      const std::string &t = *s;
      std::size_t dot = t.find('.');
      std::string whole = t.substr(0, dot), frac = dot == std::string::npos ? "" : t.substr(dot + 1);
      if (static_cast<int>(frac.size()) > scale)
        return ArgFailed(i, "'" + t + "' has more fractional digits than the scale " + std::to_string(scale), true);
      frac.append(static_cast<std::size_t>(scale) - frac.size(), '0');
      out = std::stoll(whole + frac);
      return true;
    }
    core::AuxRow row;
    row.values.push_back(*v);
    out = I64(row, 0, scale);
    return true;
  } catch (const std::exception &e) {
    return ArgFailed(i, e.what(), OutsideType(e));
  }
}

bool ArgDate(const core::QueryArguments &args, std::size_t i, int32_t &out) {
  const auto *v = Arg(args, i);
  if (!v)
    return ArgFailed(i, "NULL", true);
  try {
    core::AuxRow row;
    row.values.push_back(*v);
    out = Date(row, 0);
    return true;
  } catch (const std::exception &e) {
    return ArgFailed(i, e.what(), OutsideType(e));
  }
}

// ---- results ----
core::AuxValue OutText(const uint8_t *bytes, std::size_t len) {
  return core::AuxValue::Of(core::AuxType::String, std::string(reinterpret_cast<const char *>(bytes), len));
}
core::AuxValue OutI64(int64_t v) { return core::AuxValue::Of(core::AuxType::Int64, v); }
core::AuxValue OutDate(int32_t days) { return core::AuxValue::Of(core::AuxType::Date, core::AuxDate{days}); }
core::AuxValue OutBool(uint8_t v) { return core::AuxValue::Of(core::AuxType::Boolean, v != 0); }

core::AuxValue OutF64(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, sizeof d);
  return core::AuxValue::Of(core::AuxType::Double, d);
}

core::AuxValue OutTs(int64_t ns) {
  int64_t days = ns / 86400000000000LL, rem = ns % 86400000000000LL;
  if (rem < 0) {
    rem += 86400000000000LL;
    --days;
  }
  int64_t y = 0, m = 0, d = 0;
  CivilFromDays(days, y, m, d);
  const int64_t secs = rem / 1000000000LL, frac = rem % 1000000000LL;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld.%09lld", static_cast<long long>(y),
                static_cast<long long>(m), static_cast<long long>(d), static_cast<long long>(secs / 3600),
                static_cast<long long>(secs / 60 % 60), static_cast<long long>(secs % 60), static_cast<long long>(frac));
  return core::AuxValue::Of(core::AuxType::String, std::string(buf));
}

namespace {
__int128 I128(int64_t hi, uint64_t lo) {
  return static_cast<__int128>((static_cast<unsigned __int128>(static_cast<uint64_t>(hi)) << 64) | lo);
}
} // namespace

core::AuxValue OutDec(int64_t hi, uint64_t lo, int scale) {
  __int128 v = I128(hi, lo);
  if (v >= INT64_MIN && v <= INT64_MAX) { // the common case: an exact scaled integer, no text round trip
    core::ScaledDecimal d;
    d.value = static_cast<int64_t>(v);
    d.scale = scale;
    return core::AuxValue::Of(core::AuxType::ScaledDecimal, d);
  }
  return core::AuxValue::Of(core::AuxType::ScaledDecimal128, core::ScaledDecimal128{v, scale});
}

core::AuxValue OutRat(int64_t num_hi, uint64_t num_lo, int64_t den_hi, uint64_t den_lo) {
  const long double num = static_cast<long double>(I128(num_hi, num_lo));
  const long double den = static_cast<long double>(I128(den_hi, den_lo));
  if (den == 0) // the engine's DOUBLE division: +-inf, and 0/0 is -NaN (the x86 default NaN)
    return core::AuxValue::Of(core::AuxType::Double, num > 0 ? HUGE_VAL : num < 0 ? -HUGE_VAL : -std::numeric_limits<double>::quiet_NaN());
  return core::AuxValue::Of(core::AuxType::Double, static_cast<double>(num / den));
}

core::AuxValue OutNull(core::AuxType type) { return core::AuxValue::Null(type); }

// ---- host ----
std::string Host::Quote(const std::string &identifier) const {
  std::string out = "\"";
  for (char c : identifier) {
    if (c == '"')
      out.push_back('"');
    out.push_back(c);
  }
  return out + "\"";
}

std::string Host::QuoteText(const std::string &text) const {
  std::string out = "'";
  for (char c : text) {
    if (c == '\'')
      out.push_back('\'');
    out.push_back(c);
  }
  return out + "'";
}

std::string Host::ArrayLiteral(const std::vector<std::string> &items, const std::string &type) const {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i)
    out += (i ? "," : "") + items[i];
  return out + "]::" + type + "[]";
}

// the glue's statement becomes a subquery: no trailing `;` (or whitespace)
static std::string AsSubquery(const std::string &sql_text) {
  std::string sql = sql_text;
  while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back()))))
    sql.pop_back();
  return sql;
}

void QueryKeys(Host &host, const std::string &sql_text, const std::string &key_column,
               const std::vector<int64_t> &keys, const RowFn &row) {
  const std::string sql = AsSubquery(sql_text);
  if (keys.empty())
    return;
  // Engines with a parameter path bind the list as one BIGINT[] value instead
  // of parsing a literal of keys.size() constants.
  if (host.QueryBigintList("SELECT verus_kit_rescan.* FROM (" + sql + ") AS verus_kit_rescan WHERE verus_kit_rescan." +
                               host.Quote(key_column) + " IN (SELECT unnest($1::BIGINT[]))",
                           keys, row))
    return;
  // One query regardless of key count: semi-join the subquery against the key list, so the base
  // relation is scanned once (a chunked IN-list rescans it once per chunk). SEMI semantics keep
  // "each matching row exactly once" even when `keys` has duplicates.
  std::string list = "[";
  list.reserve(keys.size() * 9 + 2);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i)
      list += ',';
    list += std::to_string(keys[i]);
  }
  list += "]::BIGINT[]";
  host.Query("SELECT verus_kit_rescan.* FROM (" + sql + ") AS verus_kit_rescan WHERE verus_kit_rescan." +
                 host.Quote(key_column) + " IN (SELECT unnest(" + list + "))",
             row);
}

void QueryKeys(Host &host, const std::string &sql_text, const std::string &key_column,
               const std::vector<std::string> &keys, const RowFn &row) {
  if (keys.empty())
    return;
  // one query, semi-joined like the BIGINT keys; a NULL key column never matches
  std::vector<std::string> quoted;
  quoted.reserve(keys.size());
  for (const auto &key : keys)
    quoted.push_back(host.QuoteText(key));
  const std::string list = host.ArrayLiteral(quoted, "VARCHAR");
  host.Query("SELECT verus_kit_rescan.* FROM (" + AsSubquery(sql_text) + ") AS verus_kit_rescan WHERE verus_kit_rescan." +
                 host.Quote(key_column) + " IN (SELECT unnest(" + list + "))",
             row);
}

// ---- registry ----
void Register(const Glue *glue) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  Registry()[glue->id] = glue;
}

const Glue *Find(const std::string &accelerator_id) {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  const auto it = Registry().find(accelerator_id);
  return it == Registry().end() ? nullptr : it->second;
}

std::vector<const Glue *> All() {
  std::lock_guard<std::mutex> lock(RegistryMutex());
  std::vector<const Glue *> out;
  for (const auto &[_, g] : Registry())
    out.push_back(g);
  return out;
}

std::unique_ptr<core::AuxQueryModule> MakeModule(const Glue &glue, Host &host) {
  return std::make_unique<KitModule>(glue, host);
}

std::unique_ptr<aux_state::VerusAuxStore> MakeStore(const Glue &glue, aux_state::VerusAuxStore::EpochProvider epoch,
                                                    aux_state::VerusAuxStore::FloorProvider floor) {
  if (!glue.store.new_store || !glue.prepare_apply || !glue.prepare_build || !glue.answer)
    throw std::runtime_error(std::string("ACCEL_AUX_STORE=verus: ") + glue.id +
                             " has no per-accelerator store ABI; use ACCEL_AUX_STORE=verus_global");
  return std::make_unique<KitStore>(glue, std::move(epoch), std::move(floor));
}

void RegisterWorld(const World *world) { Worlds().push_back(world); }

std::unique_ptr<aux_state::VerusAuxStore> MakeWorldStore(const Glue &glue,
                                                         aux_state::VerusAuxStore::EpochProvider epoch,
                                                         aux_state::VerusAuxStore::FloorProvider floor) {
  for (const auto *world : Worlds())
    for (const auto &kit : world->kits)
      if (std::string(kit.accelerator) == glue.id && glue.answer && glue.prepare_apply && glue.prepare_build)
        return std::make_unique<WorldKitStore>(glue, kit, WorldShared::Of(*world), std::move(epoch),
                                               std::move(floor));
  return nullptr;
}

const std::string &Fault() {
  static const std::string fault = [] {
    const char *v = std::getenv("ACCEL_VERUS_KIT_FAULT");
    return std::string(v ? v : "");
  }();
  return fault;
}

} // namespace accelerator_codegen::verus_kit
