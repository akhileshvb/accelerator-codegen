// GENERATED BACKEND
#pragma once

#include "accelerator_codegen/stateful/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// The engine-neutral store, configured for PostgreSQL: versions at
// AuxMutationEpoch (the epoch provider), statements read at their pinned
// epoch, one backend's store is used by one session (transaction-dirty flag).
// Only the newest version is kept: other backends' commits reach this store
// through rebuilds (aux_sync), not versions.
class PostgresStateStore final : public VersionedAuxStore {
public:
  using EpochProvider = VersionedAuxStore::EpochProvider;
  explicit PostgresStateStore(EpochProvider epoch_provider = {});
};

} // namespace accelerator_codegen::aux_state::engine::postgres
