#include "accelerator_codegen/stateful/postgres/aux_mutation_epoch.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {
std::atomic<core::Version> g_aux_mutation_epoch{1};
}

core::Version AuxMutationEpoch() {
  return g_aux_mutation_epoch.load(std::memory_order_acquire);
}

core::Version BumpAuxMutationEpoch() {
  return g_aux_mutation_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
}

} // namespace accelerator_codegen::aux_state::engine::postgres
