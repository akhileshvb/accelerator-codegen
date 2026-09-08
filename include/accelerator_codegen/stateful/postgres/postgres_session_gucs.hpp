// GENERATED BACKEND
#pragma once

#include <cmath>

#include "accelerator_codegen/stateful/core/aux_types.hpp"

namespace accelerator_codegen::aux_state::engine::postgres {

// Returns NaN when the session GUC is unset (fall back to env/default).
double SessionExpectedQueriesBeforeWrite();

bool SessionSingleActiveAuxEnabled();

void SetSessionExpectedQueriesBeforeWrite(double value);
void SetSessionSingleActiveAuxEnabled(bool enabled);

} // namespace accelerator_codegen::aux_state::engine::postgres
