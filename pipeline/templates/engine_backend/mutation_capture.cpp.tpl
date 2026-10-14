// GENERATED BACKEND
#include "backends/{{engine_name}}/engine/{{engine_name}}_mutation_capture.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

{{EngineName}}MutationCapture::{{EngineName}}MutationCapture(
    {{EngineName}}AuxBackend &backend_p)
    : backend(backend_p) {}

void {{EngineName}}MutationCapture::Capture(const core::MutationBatch &mutation) {
  backend.CaptureMutation(mutation);
}

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
