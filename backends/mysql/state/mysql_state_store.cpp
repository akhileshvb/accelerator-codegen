// GENERATED BACKEND
#include "backends/mysql/state/mysql_state_store.hpp"

#include <utility>

namespace accelerator_codegen::aux_state::engine::mysql {

namespace {
VersionedAuxStore::Options MysqlOptions(VersionedAuxStore::EpochProvider epoch) {
  VersionedAuxStore::Options options;
  options.epoch = std::move(epoch);
  return options;
}
} // namespace

MysqlStateStore::MysqlStateStore(EpochProvider epoch_provider)
    : VersionedAuxStore(MysqlOptions(std::move(epoch_provider))) {}

} // namespace accelerator_codegen::aux_state::engine::mysql
