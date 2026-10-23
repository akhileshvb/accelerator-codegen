// The generic host kit for Verus accelerators (see verus_kit.hpp).
#include "substrate/verus/kit/verus_kit.hpp"
#include "substrate/aux/core/aux_calibration.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
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
// A kernel state owned by the host store, or shared with a Verus store read.
struct KitImage {
  KitImage(const Glue &g, void *s, bool shared_p, void (*release_p)(const void *) = nullptr)
      : glue(g), state(s), shared(shared_p), release(release_p) {}
  ~KitImage() {
    if (!state)
      return;
    if (shared && release)
      release(state);
    else if (shared)
      glue.store.state_release(state);
    else
      glue.free_state(state);
  }
  KitImage(const KitImage &) = delete;
  KitImage &operator=(const KitImage &) = delete;
  // An owned state, handed over (to a world batch); null if shared or taken.
  void *Take() const {
    if (shared)
      return nullptr;
    void *s = state;
    state = nullptr;
    return s;
  }
  const Glue &glue;
  mutable void *state;
  bool shared;
  void (*release)(const void *);
};

// A prepared next version, from the module to the Verus store (consumed).
struct KitPrepared {
  KitPrepared(const Glue &g, void *p) : glue(g), prepared(p) {}
  ~KitPrepared() {
    if (prepared)
      glue.store.prepared_free(prepared);
  }
  KitPrepared(const KitPrepared &) = delete;
  KitPrepared &operator=(const KitPrepared &) = delete;
  void *Take() const {
    void *p = prepared;
    prepared = nullptr;
    return p;
  }
  const Glue &glue;
  mutable void *prepared;
};

constexpr core::PartitionId kState = 0;

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
class KitStore final : public VerusAuxStore {
public:
  KitStore(const Glue &g, EpochProvider epoch_p, FloorProvider floor_p)
      : glue(g), epoch(std::move(epoch_p)), floor(std::move(floor_p)),
        pending_wait_micros(core::PendingWaitMicrosFromEnv()), store(g.store.new_store()) {}
  ~KitStore() override { glue.store.free_store(store); }

  const void *Handle() const { return store; }
  const Glue &GlueOf() const { return glue; }

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
      glue.store.state_release(s);
    return status == 0 ? 1 : 0;
  }
  std::size_t TotalPartitionCount() const override { return 1; }
  std::size_t TotalVersionCount() const override { return glue.store.version_count(store); }
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
      // In this store's mode the module's updates are prepared versions.
      if (void *taken = image.As<KitPrepared>()->Take()) {
        if (next)
          glue.store.prepared_free(next);
        next = taken;
      }
    }
    if (!next && invalid)
      next = glue.store.prepare_invalid(store);
    if (!next) {
      if (end_commit)
        glue.store.abort(store);
      return;
    }
    if (glue.store.install(store, next, from, CurrentFloor(), end_commit ? 1 : 0) != 0)
      ++stale_installs;
  }

  const Glue &glue;
  EpochProvider epoch;
  FloorProvider floor;
  uint64_t pending_wait_micros;
  void *store;
  std::size_t stale_installs = 0;
};

// ---- the world store (ACCEL_AUX_STORE=verus_global) ----
// One verified store for every accelerator of a benchmark (world_glue.cpp).
// A commit's accelerators each stage their new state (their kernel's apply,
// from the newest version's state); the first to begin marks the commit in
// flight, each finish puts its state in one batch, and the last one installs
// the batch as ONE version (or, if any aborted, drops it).  This assembly is
// host code (trusted); the store, the batch's world and every read are
// verified (world.rs, store.rs).
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

  void Begin(VerusAuxStore::Epoch from) {
    std::lock_guard<std::mutex> lock(mutex);
    if (participants == 0 && !world.begin(store, from))
      throw std::logic_error(std::string("concurrent commits on the Verus world ") + world.bench);
    ++participants;
  }

  // set: the accelerator's slot changes (state, or null for invalid);
  // otherwise it only took part.
  void Finish(const WorldKitOps &kit, bool set, void *state, VerusAuxStore::Epoch from, VerusAuxStore::Epoch floor) {
    std::lock_guard<std::mutex> lock(mutex);
    if (set)
      Set(kit, state);
    if (++finished < participants)
      return;
    if (aborted || !batch) {
      if (batch)
        world.batch_free(batch);
      world.abort(store);
    } else if (world.install(store, batch, from, floor, 1) != 0) {
      ++stale;
    }
    Reset();
  }

  void Abort() {
    std::lock_guard<std::mutex> lock(mutex);
    aborted = true;
    if (++finished < participants)
      return;
    if (batch)
      world.batch_free(batch);
    world.abort(store);
    Reset();
  }

  // A version outside a commit (a build): this accelerator's slot only.
  void InstallNow(const WorldKitOps &kit, void *state, VerusAuxStore::Epoch from, VerusAuxStore::Epoch floor) {
    std::lock_guard<std::mutex> lock(mutex);
    void *b = world.batch_new(store);
    if (!b) {
      if (state)
        kit.release(state); // never reached: a build's state is owned (see Take)
      return;
    }
    kit.batch_set(b, state);
    if (world.install(store, b, from, floor, 0) != 0)
      ++stale;
  }

  const World &world;
  void *store;
  std::size_t stale = 0;

private:
  void Set(const WorldKitOps &kit, void *state) {
    if (!batch)
      batch = world.batch_new(store);
    if (batch)
      kit.batch_set(batch, state);
  }
  void Reset() {
    batch = nullptr;
    participants = finished = 0;
    aborted = false;
  }

  // mutex guards the commit assembly below (shared by every accelerator of the
  // world): a leaf lock, taken inside the accelerators' commit locks.
  std::mutex mutex;
  int participants = 0, finished = 0;
  bool aborted = false;
  void *batch = nullptr;
};

class WorldKitStore final : public VerusAuxStore {
public:
  WorldKitStore(const Glue &g, const WorldKitOps &k, WorldShared &w, EpochProvider epoch_p, FloorProvider floor_p)
      : glue(g), kit(k), world(w), epoch(std::move(epoch_p)), floor(std::move(floor_p)),
        pending_wait_micros(core::PendingWaitMicrosFromEnv()) {}

  const Glue &GlueOf() const { return glue; }

  // The newest version's state: what this accelerator's next state is
  // applied to (the batch is assembled from the newest version).
  std::shared_ptr<const KitImage> Newest() const {
    const void *s = kit.newest(world.store);
    if (!s)
      return nullptr;
    return std::make_shared<const KitImage>(glue, const_cast<void *>(s), /*shared=*/true, kit.release);
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
      kit.invalidate(world.store);
  }

  bool TryPublish(const core::StagedMaintenanceOutcome &staged, core::Version) override {
    bool set = false;
    void *state = Owned(staged.invalidations, staged.updates, set);
    if (set)
      world.InstallNow(kit, state, CurrentEpoch(), CurrentFloor());
    return true;
  }

  void BeginCommit(const core::AcceleratorId &, const core::InvalidationPlan &) override {
    world.Begin(CurrentEpoch());
  }

  void FinishCommit(const core::AcceleratorId &, const core::InvalidationPlan &invalidate,
                    const std::vector<core::StateImage> &updates, core::Version) override {
    bool set = false;
    void *state = Owned(invalidate, updates, set);
    world.Finish(kit, set, state, CurrentEpoch(), CurrentFloor());
  }

  void AbortCommit(const core::AcceleratorId &) override { world.Abort(); }

  void Install(std::vector<core::StateImage> images, const core::AcceleratorId &, Epoch from_epoch) override {
    bool set = false;
    void *state = Owned(core::InvalidationPlan{}, images, set);
    if (set)
      world.InstallNow(kit, state, from_epoch, CurrentFloor());
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
  std::size_t StaleInstalls() const override { return world.stale; }

private:
  Epoch CurrentEpoch() const { return epoch ? epoch() : 0; }
  Epoch CurrentFloor() const { return floor ? floor() : std::numeric_limits<Epoch>::max(); }

  // The slot's change: the last update's state (owned, handed over), or
  // invalid; `set` false if the commit leaves the slot as it is.
  static void *Owned(const core::InvalidationPlan &invalidate, const std::vector<core::StateImage> &updates,
                     bool &set) {
    void *state = nullptr;
    set = !invalidate.IsEmpty();
    for (const auto &image : updates) {
      set = true;
      if (!image.payload)
        continue;
      if (void *taken = image.As<KitImage>()->Take()) {
        if (state)
          image.As<KitImage>()->glue.free_state(state);
        state = taken;
      }
    }
    return state;
  }

  // status: see ReadAwaitingPublish.
  std::shared_ptr<const KitImage> Resolve(core::Version at, int32_t &status) const {
    const core::StoreResolveTimer timer;
    const void *s = ReadAwaitingPublish(pending_wait_micros, at, status,
                                        [&](uint64_t epoch, uint64_t tries, int32_t *st) {
                                          return kit.read(world.store, epoch, tries, st);
                                        });
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
    if (const auto *vs = VerusStore(services)) {
      void *next = nullptr;
      try {
        next = glue.prepare_build(vs->Handle(), host);
      } catch (const Declined &) {
        next = glue.store.prepare_invalid(vs->Handle());
      }
      if (!next)
        throw std::runtime_error(std::string(glue.id) + ": no version can follow the newest");
      builder.Write(core::StateImage::Make(glue.id, kState, 0, std::make_shared<const KitPrepared>(glue, next)));
      builder.Finish();
      return;
    }
    void *state = nullptr;
    try {
      state = glue.build(host);
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
    const auto decision = services.ChooseMaintenance(
        {static_cast<uint64_t>(total), 1, true, false, true, false, false, core::MaintenanceAction::NoOp,
         services.ExpectedQueriesBeforeNextWrite()});
    if (decision.action == core::MaintenanceAction::RebuildPartition) {
      core::FinishRebuildAll(outcome);
      return outcome;
    }
    if (decision.action != core::MaintenanceAction::ApplyDelta) {
      outcome.action = decision.action;
      outcome.invalidations = core::AllPartitionsPlan();
      outcome.pending_rebuild = outcome.invalidations;
      return outcome;
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
      if (const auto *vs = VerusStore(services)) {
        void *base = glue.store.newest_base(vs->Handle());
        if (!base) {
          Rebuild(outcome);
          return outcome;
        }
        void *next = nullptr;
        try {
          next = glue.prepare_apply(vs->Handle(), base, changes, h, &err);
        } catch (...) {
          glue.store.base_free(base);
          throw;
        }
        glue.store.base_free(base);
        if (!next) {
          Rebuild(outcome);
          return outcome;
        }
        outcome.action = core::MaintenanceAction::ApplyDelta;
        outcome.updates.push_back(
            core::StateImage::Make(glue.id, kState, 0, std::make_shared<const KitPrepared>(glue, next)));
        return outcome;
      }
      // the world's batch is assembled from the newest version: apply to its state
      const auto *ws = WorldStore(services);
      const auto image = ws ? ws->Newest() : ReadImage(services, services.CurrentVisibility());
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
      ExecuteFallback(services, arguments, sink);
      return;
    }
    const auto image = ReadImage(services, services.CurrentVisibility());
    if (!image || !image->state) {
      ExecuteFallback(services, arguments, sink);
      return;
    }
    if (glue.execute_into) {
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
    if (!glue.execute(image->state, arguments, rows)) {
      rows.clear();
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

  const KitStore *VerusStore(core::AuxEngineServices &services) const {
    const auto *vs = dynamic_cast<const KitStore *>(&services.StateStore());
    return vs && &vs->GlueOf() == &glue ? vs : nullptr;
  }

  const WorldKitStore *WorldStore(core::AuxEngineServices &services) const {
    const auto *ws = dynamic_cast<const WorldKitStore *>(&services.StateStore());
    return ws && &ws->GlueOf() == &glue ? ws : nullptr;
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

const std::string &Text(const core::AuxRow &row, std::size_t i) {
  const auto &v = Cell(row, i);
  if (const auto *x = std::get_if<std::string>(&v.payload))
    return *x;
  throw Declined("not a text cell");
}

// ---- arguments ----
namespace {
std::mutex g_arg_failure_mutex;
ArgFailure g_arg_failure;

// outside_type: the value is not one of the parameter's type (NULL, more digits than its scale,
// beyond its range), so SQL answers it; otherwise the host failed to convert an in-type value.
bool ArgFailed(std::size_t i, const std::string &why, bool outside_type) {
  std::lock_guard<std::mutex> lock(g_arg_failure_mutex);
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
  std::lock_guard<std::mutex> lock(g_arg_failure_mutex);
  g_arg_failure = {};
}

ArgFailure LastArgFailure() {
  std::lock_guard<std::mutex> lock(g_arg_failure_mutex);
  return g_arg_failure;
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

void QueryKeys(Host &host, const std::string &sql_text, const std::string &key_column,
               const std::vector<int64_t> &keys, const RowFn &row) {
  // the glue's statement becomes a subquery: no trailing `;` (or whitespace)
  std::string sql = sql_text;
  while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back()))))
    sql.pop_back();
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
  if (!glue.store.new_store || !glue.prepare_apply || !glue.prepare_build)
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
      if (std::string(kit.accelerator) == glue.id)
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
