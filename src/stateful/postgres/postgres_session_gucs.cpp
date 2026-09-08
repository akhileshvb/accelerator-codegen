// GENERATED BACKEND
#include "accelerator_codegen/stateful/postgres/postgres_session_gucs.hpp"

#include <cmath>

namespace accelerator_codegen::aux_state::engine::postgres {

namespace {

double expected_queries_before_write_override = NAN;
bool single_active_aux_enabled = false;

} // namespace

void SetSessionExpectedQueriesBeforeWrite(double value) {
  expected_queries_before_write_override = value;
}

void SetSessionSingleActiveAuxEnabled(bool enabled) {
  single_active_aux_enabled = enabled;
}

double SessionExpectedQueriesBeforeWrite() {
  return expected_queries_before_write_override;
}

bool SessionSingleActiveAuxEnabled() { return single_active_aux_enabled; }

} // namespace accelerator_codegen::aux_state::engine::postgres
