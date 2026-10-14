// GENERATED BACKEND
#pragma once

#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::mysql {

// The engine-neutral store, configured for MySQL: versions at the epoch
// provider's epoch if there is one. MySQL statements read the newest state
// (no snapshot epoch), one version per query (AuxReadScope).
class MysqlStateStore final : public VersionedAuxStore {
public:
  using EpochProvider = VersionedAuxStore::EpochProvider;
  explicit MysqlStateStore(EpochProvider epoch_provider = {});
};

} // namespace accelerator_codegen::aux_state::engine::mysql
