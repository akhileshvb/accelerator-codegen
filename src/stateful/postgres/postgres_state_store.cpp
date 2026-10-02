// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"

#include <utility>

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {
VersionedAuxStore::Options PostgresOptions(VersionedAuxStore::EpochProvider epoch) {
  VersionedAuxStore::Options options;
  options.epoch = std::move(epoch);
  options.track_transaction_dirty = true;
  return options;
}
} // namespace

PostgresStateStore::PostgresStateStore(EpochProvider epoch_provider)
    : VersionedAuxStore(PostgresOptions(std::move(epoch_provider))) {}

} // namespace accelerator_codegen::aux_state::engine::postgres
