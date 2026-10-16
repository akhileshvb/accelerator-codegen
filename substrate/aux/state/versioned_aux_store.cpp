// SCAFFOLDING
// VersionedAuxStore: multi-version partition store, commit protocol, async reclaim.
#include "substrate/aux/state/versioned_aux_store.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state {

namespace {
// Superseded versions are destroyed off the commit's critical path: freeing a
// large state image (hundreds of ms for a big rebuilt aggregate) must not
// delay the writer that has already published its new version. One lazily
// started worker releases the batches in order; the destructor drains it, so
// nothing outlives the process. ACCEL_RECLAIM_ASYNC=0 destroys inline.
class VersionReclaimer {
public:
  static VersionReclaimer &Instance() {
    static VersionReclaimer instance;
    return instance;
  }
  static bool Enabled() {
    static const bool enabled = [] {
      const char *env = std::getenv("ACCEL_RECLAIM_ASYNC");
      return !(env && env[0] == '0');
    }();
    return enabled;
  }
  uint64_t Reclaimed() const { return reclaimed.load(); }
  // Returns once every batch deferred so far is destroyed.
  void Drain() {
    std::unique_lock<std::mutex> lock(mutex);
    idle_cv.wait(lock, [this] { return queue.empty() && !running; });
  }
  void Defer(std::shared_ptr<void> batch) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!worker.joinable())
        worker = std::thread([this] { Run(); });
      queue.push_back(std::move(batch));
    }
    cv.notify_one();
  }
  ~VersionReclaimer() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_one();
    if (worker.joinable())
      worker.join();
  }

private:
  void Run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      cv.wait(lock, [this] { return stop || !queue.empty(); });
      if (queue.empty())
        return; // stop and drained
      auto batch = std::move(queue.front());
      queue.pop_front();
      running = true;
      lock.unlock();
      batch.reset();
      lock.lock();
      running = false;
      reclaimed.fetch_add(1);
      idle_cv.notify_all();
    }
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::condition_variable idle_cv;
  std::atomic<uint64_t> reclaimed {0};
  bool running = false;
  std::deque<std::shared_ptr<void>> queue;
  std::thread worker;
  bool stop = false;
};

// Readers wait for a pending publish (no timeout) unless
// ACCEL_AUX_PENDING_WAIT_US bounds the wait.
uint64_t DefaultPendingWaitMicros() {
  const char *value = std::getenv("ACCEL_AUX_PENDING_WAIT_US");
  if (value && *value) {
    return std::strtoull(value, nullptr, 10);
  }
  return VersionedAuxStore::kWaitForPublish;
}

thread_local AuxReadScope *t_scope = nullptr;

// Marks the partitions `plan` names (all of them, or a set) invalid in `map`;
// `create_missing` adds the named ones that `map` lacks, as invalid.
void InvalidateIn(VersionedAuxStore::PartitionMap &map, const InvalidationPlan &plan,
                  bool create_missing) {
  auto mark = [](VersionedAuxStore::Partition &p) {
    p.valid = false;
    p.payload = nullptr;
    p.partition_version++;
  };
  if (plan.scope == InvalidationScope::AllPartitions) {
    for (auto &entry : map) {
      mark(entry.second);
    }
    return;
  }
  if (plan.scope != InvalidationScope::PartitionSet) {
    return;
  }
  for (auto key : plan.partitions) {
    auto it = map.find(key);
    if (it != map.end()) {
      mark(it->second);
    } else if (create_missing) {
      map[key] = VersionedAuxStore::Partition {};
    }
  }
}

} // namespace

WriterPreferringSharedMutex::WriterPreferringSharedMutex() {
  pthread_rwlockattr_t attr;
  pthread_rwlockattr_init(&attr);
  pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
  if (pthread_rwlock_init(&rwlock, &attr) != 0) {
    pthread_rwlockattr_destroy(&attr);
    throw std::runtime_error("pthread_rwlock_init failed");
  }
  pthread_rwlockattr_destroy(&attr);
}

WriterPreferringSharedMutex::~WriterPreferringSharedMutex() { pthread_rwlock_destroy(&rwlock); }
void WriterPreferringSharedMutex::lock() { pthread_rwlock_wrlock(&rwlock); }
bool WriterPreferringSharedMutex::try_lock() { return pthread_rwlock_trywrlock(&rwlock) == 0; }
void WriterPreferringSharedMutex::unlock() { pthread_rwlock_unlock(&rwlock); }
void WriterPreferringSharedMutex::lock_shared() { pthread_rwlock_rdlock(&rwlock); }
bool WriterPreferringSharedMutex::try_lock_shared() { return pthread_rwlock_tryrdlock(&rwlock) == 0; }
void WriterPreferringSharedMutex::unlock_shared() { pthread_rwlock_unlock(&rwlock); }

AuxReadScope::AuxReadScope() : outermost(t_scope == nullptr) {
  if (outermost) {
    t_scope = this;
  }
}

AuxReadScope::~AuxReadScope() {
  if (outermost) {
    t_scope = nullptr;
  }
}

AuxReadScope *AuxReadScope::Current() { return t_scope; }

VersionedAuxStore::VersionedAuxStore(Options options_p)
    : options(std::move(options_p)), pending_wait_micros(DefaultPendingWaitMicros()) {}

VersionedAuxStore::Epoch VersionedAuxStore::CurrentEpoch() const {
  // Providers are called without the store lock (they may take the engine's
  // own locks).
  return options.epoch ? options.epoch() : kAllSnapshots;
}

VersionedAuxStore::Epoch VersionedAuxStore::CurrentFloor() const {
  return options.floor ? options.floor() : std::numeric_limits<Epoch>::max();
}

core::AcceleratorId VersionedAuxStore::AcceleratorFor(const core::StateImage &image) const {
  if (options.fixed_accelerator) {
    return *options.fixed_accelerator;
  }
  return image.accelerator;
}

void VersionedAuxStore::PartitionSet::Finalize() {
  invalid_count = 0;
  max_version = 0;
  for (const auto &entry : map) {
    if (!entry.second.valid) {
      ++invalid_count;
    }
    max_version = std::max(max_version, entry.second.partition_version);
  }
}

namespace {
std::shared_ptr<const VersionedAuxStore::PartitionSet> MakeSet(VersionedAuxStore::PartitionMap &&map) {
  auto set = std::make_shared<VersionedAuxStore::PartitionSet>();
  set->map = std::move(map);
  set->Finalize();
  return set;
}
} // namespace

void VersionedAuxStore::RefreshHintLocked() {
  std::size_t n = 0;
  for (const auto &entry : accelerators) {
    if (!entry.second.versions.empty()) {
      n += entry.second.versions.back().partitions->map.size();
    }
  }
  total_partitions_hint.store(n, std::memory_order_relaxed);
}

const core::AcceleratorId *VersionedAuxStore::OwnerOfLocked(PartitionKey key) const {
  if (options.fixed_accelerator) {
    return &*options.fixed_accelerator;
  }
  auto it = owner.find(key);
  if (it == owner.end()) {
    return nullptr;
  }
  return &it->second;
}

const VersionedAuxStore::PartitionSet *
VersionedAuxStore::NewestLocked(const core::AcceleratorId &accelerator) const {
  auto it = accelerators.find(accelerator);
  if (it == accelerators.end() || it->second.versions.empty()) {
    return nullptr;
  }
  return it->second.versions.back().partitions.get();
}

bool VersionedAuxStore::MustWaitLocked(const core::AcceleratorId &accelerator, Epoch epoch) const {
  auto it = accelerators.find(accelerator);
  if (it == accelerators.end() || !it->second.pending) {
    return false;
  }
  // A snapshot older than the pending commit cannot include it.
  return epoch == kNewestSnapshot || epoch >= it->second.pending_from;
}

VersionedAuxStore::Resolved
VersionedAuxStore::ResolveLocked(std::shared_lock<WriterPreferringSharedMutex> &lock,
                                 const core::AcceleratorId &accelerator, Epoch epoch) const {
  const core::StoreResolveTimer timer;
  auto *scope = AuxReadScope::Current();
  if (scope) {
    for (const auto &pin : scope->pins) {
      if (pin.store == this && pin.accelerator == accelerator) {
        return {Resolve::Found, pin.partitions}; // no lock needed
      }
    }
  }
  if (!lock.owns_lock()) {
    lock.lock();
  }
  if (pending_wait_micros == kWaitForPublish) {
    pending_cv.wait(lock, [&] { return !MustWaitLocked(accelerator, epoch); });
  } else if (MustWaitLocked(accelerator, epoch)) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(pending_wait_micros);
    while (MustWaitLocked(accelerator, epoch)) {
      if (pending_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
        break;
      }
    }
  }
  auto it = accelerators.find(accelerator);
  if (it == accelerators.end()) {
    return {Resolve::Unknown, nullptr};
  }
  if (MustWaitLocked(accelerator, epoch)) {
    return {Resolve::NotVisible, nullptr}; // the wait timed out
  }
  const auto &versions = it->second.versions;
  if (versions.empty()) {
    return {Resolve::Unknown, nullptr};
  }
  const StateVersion *selected = nullptr;
  if (epoch == kNewestSnapshot) {
    selected = &versions.back();
  } else {
    for (auto v = versions.rbegin(); v != versions.rend(); ++v) {
      if (v->epoch <= epoch) {
        selected = &*v;
        break;
      }
    }
  }
  if (!selected) {
    return {Resolve::NotVisible, nullptr};
  }
  if (scope) {
    scope->pins.push_back({this, accelerator, selected->partitions});
  }
  return {Resolve::Found, selected->partitions};
}

void VersionedAuxStore::AppendLocked(const core::AcceleratorId &accelerator,
                                     std::shared_ptr<const PartitionSet> next, Epoch epoch,
                                     Epoch floor, std::vector<StateVersion> &dropped) {
  auto &acc = accelerators[accelerator];
  auto &versions = acc.versions;
  acc.generation++;
  if (epoch == kAllSnapshots) {
    for (auto &v : versions) {
      dropped.push_back(std::move(v));
    }
    versions.clear();
  } else {
    while (!versions.empty() && versions.back().epoch >= epoch) {
      dropped.push_back(std::move(versions.back()));
      versions.pop_back();
    }
  }
  versions.push_back(StateVersion {epoch, std::move(next)});
  // Keep the version a snapshot at the floor selects, and everything newer.
  std::size_t keep_from = 0;
  for (std::size_t i = 0; i < versions.size(); ++i) {
    if (versions[i].epoch <= floor) {
      keep_from = i;
    }
  }
  for (std::size_t i = 0; i < keep_from; ++i) {
    dropped.push_back(std::move(versions[i]));
  }
  versions.erase(versions.begin(), versions.begin() + static_cast<std::ptrdiff_t>(keep_from));
  EnforceVersionCapLocked(accelerator, dropped);
  RefreshHintLocked();
}

void VersionedAuxStore::EnforceVersionCapLocked(const core::AcceleratorId &accelerator,
                                                std::vector<StateVersion> &dropped) {
  if (options.max_version_bytes == 0 || !options.payload_bytes) {
    return;
  }
  auto &versions = accelerators[accelerator].versions;
  if (versions.size() < 2) {
    return;
  }
  // Exclusive bytes of each old version: payloads no newer version shares
  // (shared partitions stay alive as long as a newer version holds them).
  std::vector<uint64_t> exclusive(versions.size() - 1, 0);
  std::unordered_set<const void *> newer;
  for (const auto &entry : versions.back().partitions->map) {
    newer.insert(entry.second.payload.get());
  }
  uint64_t total = 0;
  for (std::size_t i = versions.size() - 1; i-- > 0;) {
    for (const auto &entry : versions[i].partitions->map) {
      const auto &payload = entry.second.payload;
      if (payload && newer.insert(payload.get()).second) {
        exclusive[i] += options.payload_bytes(payload);
      }
    }
    total += exclusive[i];
  }
  std::size_t drop = 0;
  while (drop < exclusive.size() && total > options.max_version_bytes) {
    total -= exclusive[drop++];
  }
  if (drop == 0) {
    return;
  }
  for (std::size_t i = 0; i < drop; ++i) {
    dropped.push_back(std::move(versions[i]));
  }
  versions.erase(versions.begin(), versions.begin() + static_cast<std::ptrdiff_t>(drop));
  versions_dropped_by_cap.fetch_add(drop, std::memory_order_relaxed);
}

void VersionedAuxStore::Reclaim(std::vector<StateVersion> &&dropped,
                                std::shared_ptr<const PartitionSet> extra) {
  if (dropped.empty() && !extra) {
    return;
  }
  if (!options.async_reclaim || !VersionReclaimer::Enabled()) {
    return; // `dropped` (and `extra`) are destroyed here, off the lock
  }
  struct Garbage {
    std::vector<StateVersion> versions;
    std::shared_ptr<const PartitionSet> base;
  };
  VersionReclaimer::Instance().Defer(
      std::make_shared<Garbage>(Garbage{std::move(dropped), std::move(extra)}));
}

uint64_t VersionedAuxStore::ReclaimedBatchCount() { return VersionReclaimer::Instance().Reclaimed(); }
void VersionedAuxStore::DrainReclaimer() { VersionReclaimer::Instance().Drain(); }

// ---- Reads ----

core::StateSnapshot VersionedAuxStore::Read(const core::VisibilityToken &visibility) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex);
  std::vector<core::AcceleratorId> names;
  for (const auto &entry : accelerators) {
    names.push_back(entry.first);
  }
  for (const auto &name : names) {
    const auto r = ResolveLocked(lock, name, visibility.mutation_epoch);
    if (r.kind != Resolve::Found) {
      continue;
    }
    for (const auto &entry : r.partitions->map) {
      if (!entry.second.valid) {
        continue;
      }
      core::StateImage image;
      image.accelerator = name;
      image.partition = static_cast<core::PartitionId>(entry.first);
      image.version = entry.second.partition_version;
      image.payload = entry.second.payload;
      snapshot.partitions.push_back(std::move(image));
    }
  }
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::StateSnapshot
VersionedAuxStore::ReadPartitions(const core::VisibilityToken &visibility,
                                  const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  snapshot.partitions.reserve(partitions.size());
  // With a fixed accelerator the owner map is not consulted, so the lock is
  // taken lazily (not at all if the scope has already pinned the version).
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex, std::defer_lock);
  if (!options.fixed_accelerator) {
    lock.lock();
  }
  // Resolutions per accelerator; a read touches one accelerator in practice.
  std::vector<std::pair<const core::AcceleratorId *, Resolved>> resolved;
  for (auto partition_id : partitions) {
    const auto key = static_cast<PartitionKey>(partition_id);
    const core::AcceleratorId *name = OwnerOfLocked(key);
    if (!name) {
      continue;
    }
    Resolved *res = nullptr;
    for (auto &entry : resolved) {
      if (entry.first == name || *entry.first == *name) {
        res = &entry.second;
        break;
      }
    }
    if (!res) {
      resolved.emplace_back(name, ResolveLocked(lock, *name, visibility.mutation_epoch));
      res = &resolved.back().second;
    }
    if (res->kind != Resolve::Found) {
      continue;
    }
    auto p = res->partitions->map.find(key);
    if (p == res->partitions->map.end() || !p->second.valid) {
      continue;
    }
    core::StateImage image;
    image.accelerator = *name;
    image.partition = partition_id;
    image.version = p->second.partition_version;
    image.payload = p->second.payload;
    snapshot.partitions.push_back(std::move(image));
  }
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::StateSnapshot
VersionedAuxStore::ReadHitPartitions(const core::VisibilityToken &visibility,
                                     const core::QueryKey &accel_key,
                                     const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  snapshot.available = false;
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex, std::defer_lock);
  const auto r = ResolveLocked(lock, accel_key.accelerator, visibility.mutation_epoch);
  if (lock.owns_lock()) {
    lock.unlock(); // r holds the immutable version
  }
  if (r.kind != Resolve::Found || r.partitions->map.empty()) {
    return snapshot;
  }
  if (r.partitions->invalid_count != 0) {
    return snapshot; // the state is not usable as a whole
  }
  snapshot.partitions.reserve(partitions.size());
  for (auto partition_id : partitions) {
    auto p = r.partitions->map.find(static_cast<PartitionKey>(partition_id));
    if (p == r.partitions->map.end()) {
      snapshot.partitions.clear();
      return snapshot;
    }
    core::StateImage image;
    image.accelerator = accel_key.accelerator;
    image.partition = partition_id;
    image.version = p->second.partition_version;
    image.payload = p->second.payload;
    snapshot.partitions.push_back(std::move(image));
  }
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::ValidityResult VersionedAuxStore::CheckValidity(const core::QueryKey &key,
                                                      const core::VisibilityToken &visibility) const {
  core::ValidityResult result;
  result.code = core::ValidityCode::Missing;
  if (options.fixed_accelerator && key.accelerator != *options.fixed_accelerator) {
    return result;
  }
  std::optional<PartitionKey> partition;
  if (!key.value.empty()) {
    core::PartitionId id = 0;
    auto parsed = std::from_chars(key.value.data(), key.value.data() + key.value.size(), id);
    if (parsed.ec != std::errc {}) {
      return result;
    }
    partition = static_cast<PartitionKey>(id);
  }
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex, std::defer_lock);
  const auto r = ResolveLocked(lock, key.accelerator, visibility.mutation_epoch);
  if (lock.owns_lock()) {
    lock.unlock(); // r holds the immutable version
  }
  if (r.kind == Resolve::Unknown) {
    return result;
  }
  if (r.kind == Resolve::NotVisible) {
    result.code = core::ValidityCode::NotVisible;
    return result;
  }
  if (partition) {
    auto p = r.partitions->map.find(*partition);
    if (p == r.partitions->map.end()) {
      return result;
    }
    if (!p->second.valid) {
      result.code = core::ValidityCode::Invalidated;
      return result;
    }
    result.code = core::ValidityCode::Valid;
    result.visible_version = p->second.partition_version;
    return result;
  }
  if (r.partitions->map.empty()) {
    return result;
  }
  if (r.partitions->invalid_count != 0) {
    result.code = core::ValidityCode::Invalidated;
    return result;
  }
  result.code = core::ValidityCode::Valid;
  result.visible_version = r.partitions->max_version;
  return result;
}

// ---- Writes outside a commit ----

void VersionedAuxStore::Invalidate(const core::InvalidationPlan &plan) {
  std::vector<std::shared_ptr<const PartitionSet>> replaced; // freed after the lock is released
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  change_count++;
  // Accelerators the plan touches: every one for AllPartitions, else the
  // owners of its partitions.
  std::vector<core::AcceleratorId> names;
  if (plan.scope == InvalidationScope::AllPartitions) {
    for (const auto &entry : accelerators) {
      names.push_back(entry.first);
    }
  } else {
    for (auto key : plan.partitions) {
      if (auto *name = OwnerOfLocked(key)) {
        if (std::find(names.begin(), names.end(), *name) == names.end()) {
          names.push_back(*name);
        }
      }
    }
  }
  for (const auto &name : names) {
    auto &acc = accelerators[name];
    auto &versions = acc.versions;
    acc.generation++;
    if (versions.empty()) {
      // Nothing published yet: record the invalid partitions (kAllSnapshots).
      PartitionMap map;
      InvalidateIn(map, plan, /*create_missing=*/true);
      if (!map.empty()) {
        versions.push_back(StateVersion {kAllSnapshots, MakeSet(std::move(map))});
      }
      continue;
    }
    for (auto &version : versions) {
      PartitionMap map = version.partitions->map;
      InvalidateIn(map, plan, /*create_missing=*/&version == &versions.back());
      replaced.push_back(std::move(version.partitions));
      version.partitions = MakeSet(std::move(map));
    }
  }
  RefreshHintLocked();
}

bool VersionedAuxStore::TryPublish(const core::StagedMaintenanceOutcome &staged,
                                   core::Version expected_version) {
  const Epoch epoch = CurrentEpoch();
  const Epoch floor = CurrentFloor();
  std::map<core::AcceleratorId, std::vector<const core::StateImage *>> groups;
  for (const auto &image : staged.updates) {
    groups[AcceleratorFor(image)].push_back(&image);
  }
  std::vector<StateVersion> dropped; // destroyed after the lock is released
  std::vector<std::shared_ptr<const PartitionSet>> replaced; // likewise
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  change_count++;
  for (const auto &group : groups) {
    const auto *newest = NewestLocked(group.first);
    PartitionMap next = newest ? newest->map : PartitionMap {};
    for (const auto *image : group.second) {
      const auto key = static_cast<PartitionKey>(image->partition);
      auto &p = next[key];
      if (expected_version != 0 && p.partition_version != expected_version) {
        // Lost the race: the partition is unusable for every snapshot.
        auto &acc = accelerators[group.first];
        acc.generation++;
        for (auto &version : acc.versions) {
          PartitionMap map = version.partitions->map;
          auto it = map.find(key);
          if (it != map.end()) {
            it->second = Partition {false, nullptr, it->second.partition_version + 1};
          }
          replaced.push_back(std::move(version.partitions));
          version.partitions = MakeSet(std::move(map));
        }
        if (acc.versions.empty()) {
          PartitionMap map;
          map[key] = Partition {false, nullptr, 1};
          acc.versions.push_back(StateVersion {kAllSnapshots, MakeSet(std::move(map))});
        }
        owner[key] = group.first;
        RefreshHintLocked();
        return false;
      }
      p = Partition {image->payload != nullptr, image->payload, p.partition_version + 1};
      owner[key] = group.first;
    }
    AppendLocked(group.first, MakeSet(std::move(next)), epoch, floor, dropped);
  }
  guard.unlock();
  Reclaim(std::move(dropped));
  return true;
}

// ---- Commits ----

void VersionedAuxStore::BeginCommit(const core::AcceleratorId &accelerator,
                                    const core::InvalidationPlan &) {
  const Epoch from = CurrentEpoch();
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  change_count++;
  auto &entry = accelerators[accelerator];
  if (entry.pending) {
    throw std::logic_error("concurrent commits on aux state of " + accelerator);
  }
  entry.pending = true;
  entry.pending_from = from;
}

void VersionedAuxStore::FinishCommit(const core::AcceleratorId &accelerator,
                                     const core::InvalidationPlan &invalidate,
                                     const std::vector<core::StateImage> &updates,
                                     core::Version) {
  const Epoch epoch = CurrentEpoch();
  const Epoch floor = CurrentFloor();
  const bool has_change = !invalidate.IsEmpty() || !updates.empty();

  // Build the next version from `base`. Pure: touches no store state.
  auto build = [&](const PartitionSet *base) {
    PartitionMap next;
    const PartitionMap *previous = nullptr;
    if (invalidate.scope == InvalidationScope::AllPartitions && !updates.empty()) {
      // A full rebuild: the updates are the whole state. Partitions it no
      // longer has are dropped, not left invalid (which would make the
      // accelerator read as invalid until the next rebuild).
      previous = base ? &base->map : nullptr;
    } else {
      if (base) {
        next = base->map;
      }
      InvalidateIn(next, invalidate, /*create_missing=*/true);
    }
    for (const auto &image : updates) {
      const auto key = static_cast<PartitionKey>(image.partition);
      auto &p = next[key];
      core::Version base_version = p.partition_version;
      if (previous) {
        auto old = previous->find(key);
        if (old != previous->end()) {
          base_version = old->second.partition_version;
        }
      }
      p = Partition {image.payload != nullptr, image.payload, base_version + 1};
    }
    return MakeSet(std::move(next));
  };

  // At most one commit per accelerator is in flight, so the newest version
  // normally cannot change while we build: copy and modify it under a shared
  // lock's snapshot of the pointer, outside any lock, and validate the
  // accelerator's generation under the exclusive lock.
  std::shared_ptr<const PartitionSet> built;
  uint64_t built_generation = 0;
  std::shared_ptr<const PartitionSet> base_ref;
  if (has_change) {
    {
      std::shared_lock<WriterPreferringSharedMutex> shared(mutex);
      auto it = accelerators.find(accelerator);
      if (it != accelerators.end()) {
        built_generation = it->second.generation;
        if (!it->second.versions.empty()) {
          base_ref = it->second.versions.back().partitions;
        }
      }
    }
    built = build(base_ref.get());
  }

  std::vector<StateVersion> dropped; // destroyed after the lock is released
  {
    std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
    change_count++;
    if (has_change) {
      auto &acc = accelerators[accelerator];
      if (acc.generation != built_generation) {
        // A concurrent change (install/invalidate) raced the build: redo it.
        const auto *newest = NewestLocked(accelerator);
        built = build(newest);
      }
      if (!options.fixed_accelerator) {
        for (const auto &image : updates) {
          owner[static_cast<PartitionKey>(image.partition)] = accelerator;
        }
      }
      AppendLocked(accelerator, std::move(built), epoch, floor, dropped);
    }
    accelerators[accelerator].pending = false;
  }
  pending_cv.notify_all();
  built.reset();
  Reclaim(std::move(dropped), std::move(base_ref));
}

void VersionedAuxStore::AbortCommit(const core::AcceleratorId &accelerator) {
  {
    std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
    change_count++;
    auto it = accelerators.find(accelerator);
    if (it == accelerators.end() || !it->second.pending) {
      return;
    }
    it->second.pending = false;
  }
  pending_cv.notify_all();
}

// ---- Builds and status ----

void VersionedAuxStore::Install(std::vector<core::StateImage> images,
                                core::AcceleratorId accelerator) {
  Install(std::move(images), std::move(accelerator), CurrentEpoch());
}

void VersionedAuxStore::Install(std::vector<core::StateImage> images,
                                core::AcceleratorId accelerator, Epoch from_epoch) {
  const Epoch floor = CurrentFloor();
  std::vector<StateVersion> dropped; // destroyed after the lock is released
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  InstallLocked(std::move(images), accelerator, from_epoch, floor, dropped);
}

bool VersionedAuxStore::InstallIfUnchanged(std::vector<core::StateImage> images,
                                           const core::AcceleratorId &accelerator,
                                           uint64_t expected_change_count) {
  const Epoch epoch = CurrentEpoch();
  const Epoch floor = CurrentFloor();
  std::vector<StateVersion> dropped; // destroyed after the lock is released
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  auto it = accelerators.find(accelerator);
  if (change_count != expected_change_count || (it != accelerators.end() && it->second.pending)) {
    return false;
  }
  InstallLocked(std::move(images), accelerator, epoch, floor, dropped);
  return true;
}

uint64_t VersionedAuxStore::ChangeCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  return change_count;
}

void VersionedAuxStore::InstallLocked(std::vector<core::StateImage> images,
                                      const core::AcceleratorId &accelerator, Epoch from_epoch,
                                      Epoch floor, std::vector<StateVersion> &dropped) {
  change_count++;
  std::map<core::AcceleratorId, PartitionMap> maps;
  for (auto &image : images) {
    const auto name = !accelerator.empty() ? accelerator : AcceleratorFor(image);
    maps[name][static_cast<PartitionKey>(image.partition)] =
        Partition {image.payload != nullptr, std::move(image.payload), 0};
  }
  if (maps.empty() && !accelerator.empty()) {
    maps[accelerator]; // a build with no partitions: an empty state
  }
  for (auto &entry : maps) {
    const auto *newest = NewestLocked(entry.first);
    for (auto &p : entry.second) {
      core::Version before = 0;
      if (newest) {
        auto it = newest->map.find(p.first);
        if (it != newest->map.end()) {
          before = it->second.partition_version;
        }
      }
      p.second.partition_version = before + 1;
      owner[p.first] = entry.first;
    }
    AppendLocked(entry.first, MakeSet(std::move(entry.second)), from_epoch, floor, dropped);
  }
}

bool VersionedAuxStore::NeedsRebuild() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  bool any = false;
  for (const auto &entry : accelerators) {
    if (entry.second.versions.empty()) {
      continue;
    }
    any = true;
    const auto &set = *entry.second.versions.back().partitions;
    if (set.map.empty() || set.invalid_count != 0) {
      return true;
    }
  }
  return !any;
}

std::size_t VersionedAuxStore::ValidPartitionCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  std::size_t n = 0;
  for (const auto &entry : accelerators) {
    if (entry.second.versions.empty()) {
      continue;
    }
    const auto &set = *entry.second.versions.back().partitions;
    n += set.map.size() - set.invalid_count;
  }
  return n;
}

std::size_t VersionedAuxStore::TotalPartitionCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  std::size_t n = 0;
  for (const auto &entry : accelerators) {
    if (!entry.second.versions.empty()) {
      n += entry.second.versions.back().partitions->map.size();
    }
  }
  return n;
}

std::size_t VersionedAuxStore::TotalVersionCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  std::size_t n = 0;
  for (const auto &entry : accelerators) {
    n += entry.second.versions.size();
  }
  return n;
}

bool VersionedAuxStore::HasPendingCommit() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  for (const auto &entry : accelerators) {
    if (entry.second.pending) {
      return true;
    }
  }
  return false;
}

void VersionedAuxStore::SetPendingWaitMicros(uint64_t micros) {
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  pending_wait_micros = micros;
}

} // namespace accelerator_codegen::aux_state
