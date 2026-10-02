#include "tpch/q21/verus/store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace accelerator_codegen::aux_state::query {

namespace {

// As VersionedAuxStore: readers wait for a pending commit without a timeout
// unless ACCEL_AUX_PENDING_WAIT_US bounds the wait.
constexpr uint64_t kWaitForPublish = ~uint64_t{0};

uint64_t PendingWaitMicros() {
  const char *value = std::getenv("ACCEL_AUX_PENDING_WAIT_US");
  if (value && *value) {
    return std::strtoull(value, nullptr, 10);
  }
  return kWaitForPublish;
}

bool Wants(const std::vector<core::PartitionId> &partitions) {
  return std::find(partitions.begin(), partitions.end(), TpchQ21VerusLayout::kState) !=
         partitions.end();
}

core::StateImage ImageOf(std::shared_ptr<const TpchQ21VerusImage> image) {
  return core::StateImage::Make(TpchQ21VerusLayout::kAccelerator, TpchQ21VerusLayout::kState,
                                0, std::move(image));
}

} // namespace

TpchQ21VerusStore::TpchQ21VerusStore(EpochProvider epoch_p, FloorProvider floor_p)
    : epoch(std::move(epoch_p)), floor(std::move(floor_p)),
      pending_wait_micros(PendingWaitMicros()), store(q21vs_new()) {}

TpchQ21VerusStore::~TpchQ21VerusStore() { q21vs_free(store); }

TpchQ21VerusStore::Epoch TpchQ21VerusStore::CurrentEpoch() const { return epoch ? epoch() : 0; }

TpchQ21VerusStore::Epoch TpchQ21VerusStore::CurrentFloor() const {
  return floor ? floor() : std::numeric_limits<Epoch>::max();
}

std::shared_ptr<const TpchQ21VerusImage>
TpchQ21VerusStore::Resolve(core::Version at, Status &status) const {
  int32_t code = 0;
  const q21v_state *s = nullptr;
  if (pending_wait_micros == kWaitForPublish) {
    s = q21vs_read(store, at, std::numeric_limits<uint64_t>::max(), &code);
  } else {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(pending_wait_micros);
    do {
      s = q21vs_read(store, at, 64, &code);
    } while (code == 2 && std::chrono::steady_clock::now() < deadline);
  }
  switch (code) {
  case 0:
    status = Status::State;
    return std::make_shared<const TpchQ21VerusImage>(TpchQ21VerusImage::Shared{}, s);
  case 1:
    status = Status::Invalid;
    break;
  case 2:
    status = Status::Pending;
    break;
  default:
    status = Status::NotVisible;
    break;
  }
  return nullptr;
}

// ---- Reads ----

core::StateSnapshot TpchQ21VerusStore::Read(const core::VisibilityToken &visibility) const {
  core::StateSnapshot snapshot;
  snapshot.accelerator = TpchQ21VerusLayout::kAccelerator;
  snapshot.visibility = visibility;
  Status status;
  if (auto image = Resolve(visibility.mutation_epoch, status)) {
    snapshot.partitions.push_back(ImageOf(std::move(image)));
  }
  snapshot.available = !snapshot.partitions.empty();
  return snapshot;
}

core::StateSnapshot
TpchQ21VerusStore::ReadPartitions(const core::VisibilityToken &visibility,
                                  const std::vector<core::PartitionId> &partitions) const {
  if (Wants(partitions)) {
    return Read(visibility);
  }
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  return snapshot;
}

core::StateSnapshot
TpchQ21VerusStore::ReadHitPartitions(const core::VisibilityToken &visibility,
                                     const core::QueryKey &key,
                                     const std::vector<core::PartitionId> &partitions) const {
  core::StateSnapshot snapshot;
  snapshot.visibility = visibility;
  if (key.accelerator != TpchQ21VerusLayout::kAccelerator || partitions.empty()) {
    return snapshot;
  }
  for (auto partition : partitions) {
    if (partition != TpchQ21VerusLayout::kState) {
      return snapshot; // the state has one partition
    }
  }
  return Read(visibility);
}

core::ValidityResult TpchQ21VerusStore::CheckValidity(const core::QueryKey &key,
                                                      const core::VisibilityToken &visibility) const {
  core::ValidityResult result;
  result.code = core::ValidityCode::Missing;
  if (key.accelerator != TpchQ21VerusLayout::kAccelerator ||
      (!key.value.empty() && key.value != "0")) {
    return result;
  }
  Status status;
  Resolve(visibility.mutation_epoch, status);
  switch (status) {
  case Status::State:
    result.code = core::ValidityCode::Valid;
    break;
  case Status::Invalid:
    result.code = core::ValidityCode::Invalidated;
    break;
  case Status::Pending:
  case Status::NotVisible:
    result.code = core::ValidityCode::NotVisible;
    break;
  }
  return result;
}

// ---- Writes ----

void TpchQ21VerusStore::InstallUpdates(const core::InvalidationPlan &invalidate,
                                       const std::vector<core::StateImage> &updates, Epoch from,
                                       bool end_commit) {
  q21vs_prepared *next = nullptr;
  bool invalid = !invalidate.IsEmpty();
  for (const auto &image : updates) {
    if (!image.payload) {
      invalid = true; // an invalid partition
      continue;
    }
    // In this store's mode the module's updates are prepared versions.
    if (auto *taken = image.As<TpchQ21VerusPrepared>()->Take()) {
      q21vs_prepared_free(next);
      next = taken;
    }
  }
  if (!next && invalid) {
    next = q21vs_prepare_invalid(store);
  }
  if (!next) {
    if (end_commit) {
      q21vs_abort(store);
    }
    return;
  }
  if (q21vs_install(store, next, from, CurrentFloor(), end_commit ? 1 : 0) != 0) {
    stale_installs++;
  }
}

void TpchQ21VerusStore::Invalidate(const core::InvalidationPlan &plan) {
  if (!plan.IsEmpty()) {
    q21vs_invalidate_all(store);
  }
}

bool TpchQ21VerusStore::TryPublish(const core::StagedMaintenanceOutcome &staged, core::Version) {
  InstallUpdates(staged.invalidations, staged.updates, CurrentEpoch(), false);
  return true;
}

void TpchQ21VerusStore::BeginCommit(const core::AcceleratorId &accelerator,
                                    const core::InvalidationPlan &) {
  if (!q21vs_begin(store, CurrentEpoch())) {
    throw std::logic_error("concurrent commits on aux state of " + accelerator);
  }
}

void TpchQ21VerusStore::FinishCommit(const core::AcceleratorId &,
                                     const core::InvalidationPlan &invalidate,
                                     const std::vector<core::StateImage> &updates, core::Version) {
  InstallUpdates(invalidate, updates, CurrentEpoch(), true);
}

void TpchQ21VerusStore::AbortCommit(const core::AcceleratorId &) { q21vs_abort(store); }

void TpchQ21VerusStore::Install(std::vector<core::StateImage> images, const core::AcceleratorId &,
                                Epoch from_epoch) {
  InstallUpdates(core::InvalidationPlan{}, images, from_epoch, false);
}

// ---- Status ----

std::size_t TpchQ21VerusStore::ValidPartitionCount() const {
  int32_t code = 0;
  const q21v_state *s = q21vs_read(store, 0, 0, &code);
  q21vs_state_release(s);
  return code == 0 ? 1 : 0;
}

std::size_t TpchQ21VerusStore::TotalPartitionCount() const { return 1; }

std::size_t TpchQ21VerusStore::TotalVersionCount() const { return q21vs_version_count(store); }

} // namespace accelerator_codegen::aux_state::query
