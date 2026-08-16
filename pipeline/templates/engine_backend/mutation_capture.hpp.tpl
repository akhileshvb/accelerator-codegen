// GENERATED BACKEND
#pragma once

#include "accelerator_codegen/stateful/{{engine_name}}/{{engine_name}}_aux_backend.hpp"

namespace accelerator_codegen::aux_state::engine::{{engine_name}} {

// Translate engine-native mutation events into MutationBatch and forward to the backend.
class {{EngineName}}MutationCapture {
public:
  explicit {{EngineName}}MutationCapture({{EngineName}}AuxBackend &backend);

  void Capture(const core::MutationBatch &mutation);

private:
  {{EngineName}}AuxBackend &backend;
};

} // namespace accelerator_codegen::aux_state::engine::{{engine_name}}
