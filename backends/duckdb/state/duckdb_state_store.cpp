// GENERATED BACKEND
#include "backends/duckdb/state/duckdb_state_store.hpp"

#include <chrono>
#include <utility>

#include "substrate/aux/core/aux_store_diagnostics.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

namespace {
VersionedAuxStore::Options DuckDBOptions(std::string accelerator_id,
                                         VersionedAuxStore::EpochProvider epoch,
                                         VersionedAuxStore::FloorProvider floor) {
  VersionedAuxStore::Options options;
  options.fixed_accelerator = std::move(accelerator_id);
  options.epoch = std::move(epoch);
  options.floor = std::move(floor);
  return options;
}
} // namespace

DuckDBStateStore::DuckDBStateStore(std::string accelerator_id, EpochProvider epoch_provider,
                                   FloorProvider floor_provider)
    : VersionedAuxStore(DuckDBOptions(std::move(accelerator_id), std::move(epoch_provider),
                                      std::move(floor_provider))) {
  // Readers wait for a pending publish (VersionedAuxStore's default).
}

core::StateSnapshot DuckDBStateStore::Read(const core::VisibilityToken &visibility) const {
  auto snapshot = VersionedAuxStore::Read(visibility);
  if (core::AuxStoreDiagnosticsEnabled()) {
    core::RecordPartitionsRead(snapshot.partitions.size(),
                               core::AuxStoreProfilingBytesEnabled() ? snapshot.partitions.size() : 0);
  }
  return snapshot;
}

core::StateSnapshot
DuckDBStateStore::ReadPartitions(const core::VisibilityToken &visibility,
                                 const std::vector<core::PartitionId> &partitions) const {
  auto snapshot = VersionedAuxStore::ReadPartitions(visibility, partitions);
  if (core::AuxStoreDiagnosticsEnabled()) {
    core::RecordPartitionsRead(snapshot.partitions.size(),
                               core::AuxStoreProfilingBytesEnabled() ? snapshot.partitions.size() : 0);
  }
  return snapshot;
}

core::ValidityResult DuckDBStateStore::CheckValidity(const core::QueryKey &key,
                                                     const core::VisibilityToken &visibility) const {
  if (!core::AuxStoreDiagnosticsEnabled()) {
    return VersionedAuxStore::CheckValidity(key, visibility);
  }
  const auto start = std::chrono::steady_clock::now();
  auto result = VersionedAuxStore::CheckValidity(key, visibility);
  const double us = static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
          .count());
  core::RecordValidityScan(TotalPartitionCountHint(), us);
  return result;
}

} // namespace accelerator_codegen::aux_state::engine::duckdb
