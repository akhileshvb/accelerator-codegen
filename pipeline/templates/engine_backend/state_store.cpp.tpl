// GENERATED BACKEND
#include "backends/{{engine_name}}/state/{{engine_name}}_state_store.hpp"

#include <utility>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

namespace {
VersionedAuxStore::Options {{EngineName}}Options(VersionedAuxStore::EpochProvider epoch,
                                                 VersionedAuxStore::FloorProvider floor) {
  VersionedAuxStore::Options options;
  options.epoch = std::move(epoch);
  options.floor = std::move(floor);
  // One store per accelerator: options.fixed_accelerator = accelerator_id;
  // One session per store: options.track_transaction_dirty = true;
  return options;
}
} // namespace

{{EngineName}}StateStore::{{EngineName}}StateStore(EpochProvider epoch_provider,
                                                   FloorProvider floor_provider)
    : VersionedAuxStore({{EngineName}}Options(std::move(epoch_provider),
                                              std::move(floor_provider))) {}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
