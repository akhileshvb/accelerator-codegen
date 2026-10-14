// GENERATED BACKEND
#pragma once

#include "backends/postgres/state/postgres_aux_backend.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Translate engine-native mutation events into MutationBatch and forward to the backend.
class PostgresMutationCapture {
public:
  explicit PostgresMutationCapture(PostgresAuxBackend &backend);

  void Capture(const core::MutationBatch &mutation);

private:
  PostgresAuxBackend &backend;
};

} // namespace accelerator_codegen::aux_state::engine::postgres
