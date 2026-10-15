// GENERATED BACKEND — {{engine_name}} host (slot implementations).
#include "backends/{{engine_name}}/api/{{engine_name}}_host.hpp"

#include <stdexcept>

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

struct {{EngineName}}Host::Impl {
  // Declared last: torn down first, so no build outlives the engine state above.
  // runtime::RuntimeHost runtime{slots, runtime::ResolveThreadBudget(<engine thread setting>)};
};

// Replace each body with the engine's real answer (see the slot contracts);
// a slot that cannot be expressed is a backend-addition request, not a workaround.
runtime::Epoch {{EngineName}}Host::PendingEpochLowerBound(uint64_t) const {
  throw std::logic_error("S2 PendingEpochLowerBound");
}
runtime::Epoch {{EngineName}}Host::OldestReaderEpoch() const { throw std::logic_error("S4 OldestReaderEpoch"); }
bool {{EngineName}}Host::WaitForPreRegistrationWriters(const std::vector<core::TableId> &, uint64_t) {
  throw std::logic_error("S5 WaitForPreRegistrationWriters");
}
std::unique_ptr<runtime::BuildSnapshot> {{EngineName}}Host::OpenBuildSnapshot(runtime::AcceleratorRuntime &) {
  throw std::logic_error("S5 OpenBuildSnapshot");
}
bool {{EngineName}}Host::EngineWorkOnHostThread() const { throw std::logic_error("S6 EngineWorkOnHostThread"); }
void {{EngineName}}Host::RunBuild(runtime::AcceleratorRuntime &, runtime::BuildSnapshot &, core::StateBuilder &) {
  throw std::logic_error("S8 RunBuild");
}
const maintenance::CostModelParams &{{EngineName}}Host::CostModel(const core::AcceleratorId &) const {
  throw std::logic_error("CostModel");
}
runtime::Epoch {{EngineName}}Host::ReaderEpoch() const { throw std::logic_error("S3 ReaderEpoch"); }

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
