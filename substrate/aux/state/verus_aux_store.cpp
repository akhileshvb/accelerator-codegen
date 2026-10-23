// Selection and construction of Verus-backed aux stores.
#include "substrate/aux/state/verus_aux_store.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include "substrate/verus/kit/verus_kit.hpp"

namespace accelerator_codegen::aux_state {

bool VerusAuxStoreRequested() {
  const char *value = std::getenv("ACCEL_AUX_STORE");
  return value && (std::string(value) == "verus" || std::string(value) == "verus_global");
}

std::unique_ptr<VerusAuxStore> MakeVerusAuxStore(const core::AcceleratorId &accelerator,
                                                 VerusAuxStore::EpochProvider epoch,
                                                 VerusAuxStore::FloorProvider floor) {
  if (const auto *glue = verus_kit::Find(accelerator)) {
    // verus_global: the one store of the world that names the accelerator
    const char *mode = std::getenv("ACCEL_AUX_STORE");
    if (mode && std::string(mode) == "verus_global") {
      if (auto store = verus_kit::MakeWorldStore(*glue, epoch, floor)) {
        return store;
      }
      throw std::runtime_error("ACCEL_AUX_STORE=verus_global: no Verus world names " + accelerator +
                               " (scripts/gen_verus_world.py, build_verus_world.sh)");
    }
    return verus_kit::MakeStore(*glue, std::move(epoch), std::move(floor));
  }
  return nullptr;
}

} // namespace accelerator_codegen::aux_state
