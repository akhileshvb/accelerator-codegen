// GENERATED BACKEND
#pragma once

#include "substrate/aux/state/versioned_aux_store.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

// The engine-neutral store (VersionedAuxStore), configured for {{EngineName}}.
// VersionedAuxStore *is* the AuxStateStore implementation (versions, pending
// commits, CAS on expected_version, invalidation, locking); this class only
// chooses its Options. Document here which engine clock each provider reads:
//   epoch: the epoch a commit/publish becomes current from (same order as
//          AuxEngineServices::CurrentVisibility().mutation_epoch);
//   floor: the oldest snapshot epoch still open (none: keep only the newest).
class {{EngineName}}StateStore final : public VersionedAuxStore {
public:
  using EpochProvider = VersionedAuxStore::EpochProvider;
  using FloorProvider = VersionedAuxStore::FloorProvider;

  explicit {{EngineName}}StateStore(EpochProvider epoch_provider = {},
                                    FloorProvider floor_provider = {});

  // Optional: override Read / ReadPartitions / CheckValidity only to wrap the
  // VersionedAuxStore:: call (e.g. diagnostics); never re-implement them.
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
