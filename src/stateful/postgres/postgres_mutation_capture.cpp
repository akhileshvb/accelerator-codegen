// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

PostgresMutationCapture::PostgresMutationCapture(
    PostgresAuxBackend &backend_p)
    : backend(backend_p) {}

void PostgresMutationCapture::Capture(const core::MutationBatch &mutation) {
  backend.CaptureMutation(mutation);
}

} // namespace accelerator_codegen::aux_state::engine::postgres
