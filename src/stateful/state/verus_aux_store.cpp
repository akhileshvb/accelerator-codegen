#include "accelerator_codegen/stateful/state/verus_aux_store.hpp"

#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include "accelerator_codegen/verus/verus_kit.hpp"

namespace accelerator_codegen::aux_state {

namespace {

std::map<std::string, VerusAuxStoreFactory> &StoreFactories() {
  static std::map<std::string, VerusAuxStoreFactory> factories;
  return factories;
}

} // namespace

void RegisterVerusAuxStore(std::string accelerator, VerusAuxStoreFactory factory) {
  StoreFactories()[std::move(accelerator)] = factory;
}

bool VerusAuxStoreRequested() {
  const char *value = std::getenv("ACCEL_AUX_STORE");
  return value && std::string(value) == "verus";
}

std::unique_ptr<VerusAuxStore> MakeVerusAuxStore(const core::AcceleratorId &accelerator,
                                                 VerusAuxStore::EpochProvider epoch,
                                                 VerusAuxStore::FloorProvider floor) {
  if (const auto *glue = verus_kit::Find(accelerator)) {
    return verus_kit::MakeStore(*glue, std::move(epoch), std::move(floor));
  }
  const auto it = StoreFactories().find(accelerator);
  if (it != StoreFactories().end()) {
    return it->second(std::move(epoch), std::move(floor));
  }
  return nullptr;
}

} // namespace accelerator_codegen::aux_state
