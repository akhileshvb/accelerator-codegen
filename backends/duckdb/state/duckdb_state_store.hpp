// GENERATED BACKEND
#pragma once

#include <string>
#include <vector>

#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::duckdb {

// The engine-neutral store, configured for DuckDB: one store per
// accelerator; versions at AuxCommitClock's epoch (commit id + 1); readers
// at their transaction's start time; versions kept back to the lowest active
// start time (the floor provider). Records the store diagnostics counters.
class DuckDBStateStore final : public VersionedAuxStore {
public:
  using EpochProvider = VersionedAuxStore::EpochProvider;
  using FloorProvider = VersionedAuxStore::FloorProvider;

  explicit DuckDBStateStore(std::string accelerator_id, EpochProvider epoch_provider = {},
                            FloorProvider floor_provider = {});

  core::StateSnapshot Read(const core::VisibilityToken &) const override;
  core::StateSnapshot ReadPartitions(const core::VisibilityToken &,
                                     const std::vector<core::PartitionId> &) const override;
  core::ValidityResult CheckValidity(const core::QueryKey &,
                                     const core::VisibilityToken &) const override;
};

} // namespace accelerator_codegen::aux_state::engine::duckdb
