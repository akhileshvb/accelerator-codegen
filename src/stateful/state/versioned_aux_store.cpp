// SCAFFOLDING
#include "accelerator_codegen/stateful/state/versioned_aux_store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state {

namespace {

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

// Mark `keys` (or every partition, if `all`) invalid in `map`.
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

std::optional<core::AcceleratorId> VersionedAuxStore::OwnerOfLocked(PartitionKey key) const {
  if (options.fixed_accelerator) {
    return options.fixed_accelerator;
  }
  auto it = owner.find(key);
  if (it == owner.end()) {
    return std::nullopt;
  }
  return it->second;
}

const VersionedAuxStore::PartitionMap *
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
  auto *scope = AuxReadScope::Current();
  const AuxReadScope::Key pin_key {this, accelerator};
  if (scope) {
    auto pinned = scope->pins.find(pin_key);
    if (pinned != scope->pins.end()) {
      return {Resolve::Found, pinned->second};
    }
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
    scope->pins[pin_key] = selected->partitions;
  }
  return {Resolve::Found, selected->partitions};
}

void VersionedAuxStore::AppendLocked(const core::AcceleratorId &accelerator, PartitionMap next,
                                     Epoch epoch, Epoch floor) {
  auto &versions = accelerators[accelerator].versions;
  if (epoch == kAllSnapshots) {
    versions.clear();
  } else {
    while (!versions.empty() && versions.back().epoch >= epoch) {
      versions.pop_back();
    }
  }
  versions.push_back(StateVersion {epoch, std::make_shared<const PartitionMap>(std::move(next))});
  // Keep the version a snapshot at the floor selects, and everything newer.
  std::size_t keep_from = 0;
  for (std::size_t i = 0; i < versions.size(); ++i) {
    if (versions[i].epoch <= floor) {
      keep_from = i;
    }
  }
  versions.erase(versions.begin(), versions.begin() + static_cast<std::ptrdiff_t>(keep_from));
}

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
    for (const auto &entry : *r.partitions) {
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
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex);
  std::map<core::AcceleratorId, Resolved> resolved;
  for (auto partition_id : partitions) {
    const auto key = static_cast<PartitionKey>(partition_id);
    const auto name = OwnerOfLocked(key);
    if (!name) {
      continue;
    }
    auto it = resolved.find(*name);
    if (it == resolved.end()) {
      it = resolved.emplace(*name, ResolveLocked(lock, *name, visibility.mutation_epoch)).first;
    }
    if (it->second.kind != Resolve::Found) {
      continue;
    }
    auto p = it->second.partitions->find(key);
    if (p == it->second.partitions->end() || !p->second.valid) {
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
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex);
  const auto r = ResolveLocked(lock, accel_key.accelerator, visibility.mutation_epoch);
  if (r.kind != Resolve::Found || r.partitions->empty()) {
    return snapshot;
  }
  for (const auto &entry : *r.partitions) {
    if (!entry.second.valid) {
      return snapshot; // the state is not usable as a whole
    }
  }
  for (auto partition_id : partitions) {
    auto p = r.partitions->find(static_cast<PartitionKey>(partition_id));
    if (p == r.partitions->end()) {
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
  std::shared_lock<WriterPreferringSharedMutex> lock(mutex);
  const auto r = ResolveLocked(lock, key.accelerator, visibility.mutation_epoch);
  if (r.kind == Resolve::Unknown) {
    return result;
  }
  if (r.kind == Resolve::NotVisible) {
    result.code = core::ValidityCode::NotVisible;
    return result;
  }
  if (partition) {
    auto p = r.partitions->find(*partition);
    if (p == r.partitions->end()) {
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
  if (r.partitions->empty()) {
    return result;
  }
  core::Version max_version = 0;
  for (const auto &entry : *r.partitions) {
    if (!entry.second.valid) {
      result.code = core::ValidityCode::Invalidated;
      return result;
    }
    max_version = std::max(max_version, entry.second.partition_version);
  }
  result.code = core::ValidityCode::Valid;
  result.visible_version = max_version;
  return result;
}

// ---- Writes outside a commit ----

void VersionedAuxStore::Invalidate(const core::InvalidationPlan &plan) {
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
      if (auto name = OwnerOfLocked(key)) {
        if (std::find(names.begin(), names.end(), *name) == names.end()) {
          names.push_back(*name);
        }
      }
    }
  }
  for (const auto &name : names) {
    auto &versions = accelerators[name].versions;
    if (versions.empty()) {
      // Nothing published yet: record the invalid partitions (kAllSnapshots).
      PartitionMap map;
      InvalidateIn(map, plan, /*create_missing=*/true);
      if (!map.empty()) {
        versions.push_back(StateVersion {kAllSnapshots, std::make_shared<const PartitionMap>(std::move(map))});
      }
      continue;
    }
    for (auto &version : versions) {
      PartitionMap map = *version.partitions;
      InvalidateIn(map, plan, /*create_missing=*/&version == &versions.back());
      version.partitions = std::make_shared<const PartitionMap>(std::move(map));
    }
  }
}

bool VersionedAuxStore::TryPublish(const core::StagedMaintenanceOutcome &staged,
                                   core::Version expected_version) {
  const Epoch epoch = CurrentEpoch();
  const Epoch floor = CurrentFloor();
  std::map<core::AcceleratorId, std::vector<const core::StateImage *>> groups;
  for (const auto &image : staged.updates) {
    groups[AcceleratorFor(image)].push_back(&image);
  }
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  change_count++;
  for (const auto &group : groups) {
    const auto *newest = NewestLocked(group.first);
    PartitionMap next = newest ? *newest : PartitionMap {};
    for (const auto *image : group.second) {
      const auto key = static_cast<PartitionKey>(image->partition);
      auto &p = next[key];
      if (expected_version != 0 && p.partition_version != expected_version) {
        // Lost the race: the partition is unusable for every snapshot.
        for (auto &version : accelerators[group.first].versions) {
          PartitionMap map = *version.partitions;
          auto it = map.find(key);
          if (it != map.end()) {
            it->second = Partition {false, nullptr, it->second.partition_version + 1};
          }
          version.partitions = std::make_shared<const PartitionMap>(std::move(map));
        }
        if (accelerators[group.first].versions.empty()) {
          PartitionMap map;
          map[key] = Partition {false, nullptr, 1};
          accelerators[group.first].versions.push_back(
              StateVersion {kAllSnapshots, std::make_shared<const PartitionMap>(std::move(map))});
        }
        owner[key] = group.first;
        return false;
      }
      p = Partition {image->payload != nullptr, image->payload, p.partition_version + 1};
      owner[key] = group.first;
    }
    AppendLocked(group.first, std::move(next), epoch, floor);
  }
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
  {
    std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
    change_count++;
    if (!invalidate.IsEmpty() || !updates.empty()) {
      const auto *newest = NewestLocked(accelerator);
      PartitionMap next = newest ? *newest : PartitionMap {};
      PartitionMap previous;
      if (invalidate.scope == InvalidationScope::AllPartitions && !updates.empty()) {
        // A full rebuild: the updates are the whole state. Partitions it no
        // longer has are dropped, not left invalid (which would make the
        // accelerator read as invalid until the next rebuild).
        previous = std::move(next);
        next = PartitionMap {};
      } else {
        InvalidateIn(next, invalidate, /*create_missing=*/true);
      }
      for (const auto &image : updates) {
        const auto key = static_cast<PartitionKey>(image.partition);
        auto &p = next[key];
        auto old = previous.find(key);
        const auto base = old != previous.end() ? old->second.partition_version : p.partition_version;
        p = Partition {image.payload != nullptr, image.payload, base + 1};
        owner[key] = accelerator;
      }
      AppendLocked(accelerator, std::move(next), epoch, floor);
    }
    accelerators[accelerator].pending = false;
  }
  pending_cv.notify_all();
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

void VersionedAuxStore::MarkTransactionDirty() {
  if (!options.track_transaction_dirty) {
    return;
  }
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  transaction_dirty = true;
}

void VersionedAuxStore::ClearTransactionDirty() {
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  transaction_dirty = false;
}

bool VersionedAuxStore::IsTransactionDirty() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  return transaction_dirty;
}

// ---- Builds and status ----

void VersionedAuxStore::Install(std::vector<core::StateImage> images,
                                core::AcceleratorId accelerator) {
  Install(std::move(images), std::move(accelerator), CurrentEpoch());
}

void VersionedAuxStore::Install(std::vector<core::StateImage> images,
                                core::AcceleratorId accelerator, Epoch from_epoch) {
  const Epoch floor = CurrentFloor();
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  InstallLocked(std::move(images), accelerator, from_epoch, floor);
}

bool VersionedAuxStore::InstallIfUnchanged(std::vector<core::StateImage> images,
                                           const core::AcceleratorId &accelerator,
                                           uint64_t expected_change_count) {
  const Epoch epoch = CurrentEpoch();
  const Epoch floor = CurrentFloor();
  std::unique_lock<WriterPreferringSharedMutex> guard(mutex);
  auto it = accelerators.find(accelerator);
  if (change_count != expected_change_count || (it != accelerators.end() && it->second.pending)) {
    return false;
  }
  InstallLocked(std::move(images), accelerator, epoch, floor);
  return true;
}

uint64_t VersionedAuxStore::ChangeCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  return change_count;
}

void VersionedAuxStore::InstallLocked(std::vector<core::StateImage> images,
                                      const core::AcceleratorId &accelerator, Epoch from_epoch,
                                      Epoch floor) {
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
        auto it = newest->find(p.first);
        if (it != newest->end()) {
          before = it->second.partition_version;
        }
      }
      p.second.partition_version = before + 1;
      owner[p.first] = entry.first;
    }
    AppendLocked(entry.first, std::move(entry.second), from_epoch, floor);
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
    const auto &map = *entry.second.versions.back().partitions;
    if (map.empty()) {
      return true;
    }
    for (const auto &p : map) {
      if (!p.second.valid) {
        return true;
      }
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
    for (const auto &p : *entry.second.versions.back().partitions) {
      n += p.second.valid ? 1 : 0;
    }
  }
  return n;
}

std::size_t VersionedAuxStore::TotalPartitionCount() const {
  std::shared_lock<WriterPreferringSharedMutex> guard(mutex);
  std::size_t n = 0;
  for (const auto &entry : accelerators) {
    if (!entry.second.versions.empty()) {
      n += entry.second.versions.back().partitions->size();
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
