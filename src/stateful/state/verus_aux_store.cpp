#include "accelerator_codegen/stateful/state/verus_aux_store.hpp"

#include <cstdlib>
#include <string>
#include <utility>

#if defined(ACCEL_HAVE_Q21_VERUS)
#include "tpch/q21/verus/store.hpp"
#endif

namespace accelerator_codegen::aux_state {

bool VerusAuxStoreRequested() {
  const char *value = std::getenv("ACCEL_AUX_STORE");
  return value && std::string(value) == "verus";
}

std::unique_ptr<VerusAuxStore> MakeVerusAuxStore(const core::AcceleratorId &accelerator,
                                                 VerusAuxStore::EpochProvider epoch,
                                                 VerusAuxStore::FloorProvider floor) {
#if defined(ACCEL_HAVE_Q21_VERUS)
  if (accelerator == query::TpchQ21VerusLayout::kAccelerator) {
    return std::make_unique<query::TpchQ21VerusStore>(std::move(epoch), std::move(floor));
  }
#endif
  (void)accelerator;
  (void)epoch;
  (void)floor;
  return nullptr;
}

} // namespace accelerator_codegen::aux_state
