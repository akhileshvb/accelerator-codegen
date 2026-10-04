// SCAFFOLDING
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <pthread.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "accelerator_codegen/stateful/core/aux_types.hpp"
#include "accelerator_codegen/stateful/state/partition_key.hpp"

namespace accelerator_codegen::aux_state {

// A reader-writer lock that prefers writers: a waiting writer blocks new
// readers, so a stream of reads cannot starve commits. (std::shared_mutex is
// glibc's default rwlock, which prefers readers.) Satisfies SharedMutex.
class WriterPreferringSharedMutex {
public:
  WriterPreferringSharedMutex();
  ~WriterPreferringSharedMutex();
  WriterPreferringSharedMutex(const WriterPreferringSharedMutex &) = delete;
  WriterPreferringSharedMutex &operator=(const WriterPreferringSharedMutex &) = delete;

  void lock();
  bool try_lock();
  void unlock();
  void lock_shared();
  bool try_lock_shared();
  void unlock_shared();

private:
  pthread_rwlock_t rwlock;
};

// Engine-neutral aux-state store (the one AuxStateStore implementation; the
// engine stores only configure it).
//
// Versions. Each accelerator's state is a list of versions, oldest first. A
// version is the accelerator's whole state (an immutable partition map,
// partitions shared between versions) and the epoch from which it is
// current. A reader at snapshot epoch E sees the newest version whose epoch
// is <= E: so every partition it reads comes from one version, and a reader
// whose snapshot predates a commit keeps reading the state of its snapshot.
//
// Epochs are the engine's: a snapshot epoch comes from
// AuxEngineServices::CurrentVisibility, a commit's epoch from the store's
// EpochProvider (see stateful/core/README.md for each engine).
//   * kNewestSnapshot (0) as a reader's epoch: "no snapshot", read the newest.
//   * kAllSnapshots (0) as a version's epoch: current for every snapshot
//     (and it replaces the history).
//
// Commits. BeginCommit marks the accelerator pending from the commit's
// epoch: a reader whose snapshot may include the commit (E >= that epoch, or
// kNewestSnapshot) waits, bounded, for FinishCommit, which appends the new
// version (invalidations and updates applied together) and ends the pending
// mark; older snapshots read on. AbortCommit ends it with no change. At most
// one commit per accelerator is in flight.
//
// Pruning. On each new version, versions no open snapshot can select any more
// are dropped: those superseded at or below the epoch floor (the oldest
// snapshot epoch in use). Without a floor provider only the newest is kept.
//
// One query, one version. Inside an AuxReadScope, the first read of an
// accelerator pins the version it resolves; later reads in the scope reuse
// it, so a query that reads several times cannot mix versions.
class VersionedAuxStore : public core::AuxStateStore {
public:
  using Epoch = core::Version;
  using EpochProvider = std::function<Epoch()>;
  using FloorProvider = std::function<Epoch()>;

  static constexpr Epoch kNewestSnapshot = 0;
  static constexpr Epoch kAllSnapshots = 0;

  struct Options {
    // The epoch of a commit (and of BeginCommit's pending mark). None: every
    // version is kAllSnapshots, and every reader waits on a pending commit.
    EpochProvider epoch;
    // The oldest snapshot epoch in use. None: keep only the newest version.
    FloorProvider floor;
    // Every image belongs to this accelerator (a store per accelerator).
    std::optional<core::AcceleratorId> fixed_accelerator;
    // Honor Mark/Clear/IsTransactionDirty (single-session engines only: the
    // flag is store-wide).
    bool track_transaction_dirty = false;
  };

  explicit VersionedAuxStore(Options options);

  // ---- AuxStateStore: reads ----
  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::StateSnapshot
  ReadPartitions(const core::VisibilityToken &,
                 const std::vector<core::PartitionId> &) const override;
  core::StateSnapshot
  ReadHitPartitions(const core::VisibilityToken &, const core::QueryKey &,
                    const std::vector<core::PartitionId> &) const override;
  // Empty QueryKey.value: the accelerator's whole state; otherwise a decimal
  // partition id.
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;

  // ---- AuxStateStore: writes outside a commit ----
  // Invalid for every snapshot (every kept version).
  void Invalidate(const core::InvalidationPlan &) override;
  // Publish `updates` as a new version at the current epoch. A partition
  // whose version is not `expected_version` (when non-zero) is invalidated
  // for every snapshot instead, and the result is false.
  bool TryPublish(const core::StagedMaintenanceOutcome &,
                  core::Version expected_version) override;

  // ---- AuxStateStore: commits ----
  void BeginCommit(const core::AcceleratorId &, const core::InvalidationPlan &) override;
  // `expected_version` is ignored: one commit per accelerator is in flight.
  void FinishCommit(const core::AcceleratorId &, const core::InvalidationPlan &invalidate,
                    const std::vector<core::StateImage> &updates,
                    core::Version expected_version = 0) override;
  void AbortCommit(const core::AcceleratorId &) override;

  void MarkTransactionDirty() override;
  void ClearTransactionDirty() override;
  bool IsTransactionDirty() const override;

  // ---- Builds and status ----
  // `accelerator`'s state becomes exactly `images` (a fresh build), current
  // from `from_epoch` (default: the epoch provider's, kAllSnapshots without
  // one). An empty `accelerator` keeps each image's own.
  void Install(std::vector<core::StateImage> images, core::AcceleratorId accelerator);
  void Install(std::vector<core::StateImage> images, core::AcceleratorId accelerator,
               Epoch from_epoch);
  // For a rebuild that may race commits (built outside any commit, from data
  // that may or may not include an in-flight write): installs only if the
  // store has not changed since ChangeCount() returned
  // `expected_change_count` and no commit of `accelerator` is pending.
  // Returns false (nothing installed) otherwise; the caller discards the build.
  bool InstallIfUnchanged(std::vector<core::StateImage> images,
                          const core::AcceleratorId &accelerator, uint64_t expected_change_count);
  // Bumped by every change (commits, installs, invalidations).
  uint64_t ChangeCount() const;
  // True if some accelerator has no newest state, or one with an invalid
  // partition (Execute should rebuild).
  bool NeedsRebuild() const;
  std::size_t ValidPartitionCount() const;
  std::size_t TotalPartitionCount() const;
  // Lock-free approximation of TotalPartitionCount (refreshed on every
  // change), for diagnostics on the read path.
  std::size_t TotalPartitionCountHint() const {
    return total_partitions_hint.load(std::memory_order_relaxed);
  }
  std::size_t TotalVersionCount() const;
  bool HasPendingCommit() const;

  // Maximum wait for a pending commit. Default kWaitForPublish (no timeout:
  // a reader waits for the publish), or ACCEL_AUX_PENDING_WAIT_US. On a
  // timeout the accelerator reads as not visible.
  static constexpr uint64_t kWaitForPublish = ~uint64_t {0};
  void SetPendingWaitMicros(uint64_t micros);

  struct Partition {
    bool valid = false;
    std::shared_ptr<const void> payload;
    core::Version partition_version = 0;
  };
  using PartitionMap = std::map<PartitionKey, Partition>;
  // An immutable partition map plus summary statistics computed once when the
  // version is built, so whole-state validity checks are O(1).
  struct PartitionSet {
    PartitionMap map;
    std::size_t invalid_count = 0; // partitions with valid == false
    core::Version max_version = 0; // max partition_version over all partitions
    void Finalize();
  };
  struct StateVersion {
    Epoch epoch = kAllSnapshots;
    std::shared_ptr<const PartitionSet> partitions;
  };

private:
  struct Accelerator {
    std::vector<StateVersion> versions; // oldest first, epochs increasing
    bool pending = false;
    Epoch pending_from = 0;
    // Bumped whenever `versions` changes; lets a commit build its next
    // version outside the lock and detect a concurrent change.
    uint64_t generation = 0;
  };

  enum class Resolve { Found, NotVisible, Unknown };
  struct Resolved {
    Resolve kind = Resolve::Unknown;
    std::shared_ptr<const PartitionSet> partitions;
  };

  core::AcceleratorId AcceleratorFor(const core::StateImage &) const;
  Epoch CurrentEpoch() const;
  Epoch CurrentFloor() const;
  // The accelerator's version at `epoch` (waits for a pending commit the
  // snapshot may include; honors the current AuxReadScope).
  // `lock` may be unlocked (deferred) on entry: it is locked here only if the
  // scope has not already pinned a version (a pin hit takes no lock).
  Resolved ResolveLocked(std::shared_lock<WriterPreferringSharedMutex> &lock,
                         const core::AcceleratorId &accelerator, Epoch epoch) const;
  bool MustWaitLocked(const core::AcceleratorId &accelerator, Epoch epoch) const;
  // Append `next` as `accelerator`'s newest version at `epoch`. Dropped
  // versions are moved into `dropped` so the caller destroys them (possibly
  // large payloads) after releasing the lock.
  void AppendLocked(const core::AcceleratorId &accelerator,
                    std::shared_ptr<const PartitionSet> next, Epoch epoch, Epoch floor,
                    std::vector<StateVersion> &dropped);
  const PartitionSet *NewestLocked(const core::AcceleratorId &accelerator) const;
  void RefreshHintLocked();
  void InstallLocked(std::vector<core::StateImage> images, const core::AcceleratorId &accelerator,
                     Epoch from_epoch, Epoch floor, std::vector<StateVersion> &dropped);
  // nullptr if unknown. Valid while the lock is held.
  const core::AcceleratorId *OwnerOfLocked(PartitionKey key) const;

  Options options;
  uint64_t pending_wait_micros;
  // Readers share it (a read changes nothing in the store); writers own it.
  mutable WriterPreferringSharedMutex mutex;
  mutable std::condition_variable_any pending_cv;
  std::unordered_map<core::AcceleratorId, Accelerator> accelerators;
  // Which accelerator published each partition key (for reads by partition).
  std::unordered_map<PartitionKey, core::AcceleratorId> owner;
  bool transaction_dirty = false;
  uint64_t change_count = 0;
  std::atomic<std::size_t> total_partitions_hint {0};
};

// Pins, per store and accelerator, the version the first read in the scope
// resolves (see VersionedAuxStore). Open one per query on the reading thread;
// nested scopes reuse the outermost.
class AuxReadScope {
public:
  AuxReadScope();
  ~AuxReadScope();
  AuxReadScope(const AuxReadScope &) = delete;
  AuxReadScope &operator=(const AuxReadScope &) = delete;

  static AuxReadScope *Current();

  struct Pin {
    const void *store;
    core::AcceleratorId accelerator;
    std::shared_ptr<const VersionedAuxStore::PartitionSet> partitions;
  };
  // A query touches a handful of (store, accelerator) pairs: linear search.
  std::vector<Pin> pins;

private:
  bool outermost;
};

} // namespace accelerator_codegen::aux_state
