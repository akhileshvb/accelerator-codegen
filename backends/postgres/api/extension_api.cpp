// GENERATED BACKEND - query-facing SQL entry points for Postgres.
// The accel_postgres_* functions declared in
// backends/postgres/engine/accelerator_codegen_postgres--0.1.sql (observer,
// forced maintenance action, execute counts, last maintenance decision).
// Per-query SQL functions register through backends/postgres/api/runtime.cpp.
// Linked into the accelerator_codegen_pg MODULE alongside
// backends/postgres/engine/extension_engine.cpp (which has PG_MODULE_MAGIC).
//
// The engine_backend `host` layer gate-test UDFs (pg_accel_host_*) are not
// here: they live in backends/postgres/tests/extension_test_api.cpp, built only
// into the test MODULE accelerator_codegen_pg_test.

#include <string>

#include "backends/postgres/state/aux_server_text.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"
#include "substrate/aux/core/aux_maintenance_decision_log.hpp"

extern "C" {

#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>

extern "C" {
void accel_pg_init_backend(const char *backing_dir);
int accel_pg_get_observer_mutation_count();
void accel_pg_reset_observer();
void accel_pg_set_force_maintenance_action(const char *action_name);
const char *accel_pg_get_last_maintenance_action(void);
}

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_mutation_count);
Datum accel_postgres_get_observer_mutation_count(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(accel_pg_get_observer_mutation_count());
}

PG_FUNCTION_INFO_V1(accel_postgres_reset_observer);
Datum accel_postgres_reset_observer(PG_FUNCTION_ARGS) {
  accel_pg_reset_observer();
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(accel_postgres_set_force_maintenance_action);
Datum accel_postgres_set_force_maintenance_action(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0)) {
    accel_pg_set_force_maintenance_action("");
  } else {
    char *cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
    accel_pg_set_force_maintenance_action(cstr);
    pfree(cstr);
  }
  PG_RETURN_VOID();
}

// How this backend's accelerator queries were answered
// ("hit=H rebuilt_on_read=R fallback=F").
PG_FUNCTION_INFO_V1(accel_postgres_get_execute_counts);
Datum accel_postgres_get_execute_counts(PG_FUNCTION_ARGS) {
  const auto text = accelerator_codegen::aux_state::core::FormatAuxExecuteCounts(
      accelerator_codegen::aux_state::core::ReadAuxExecuteCounts());
  PG_RETURN_TEXT_P(cstring_to_text(text.c_str()));
}

// The last maintenance decision in this backend
// ("action=A rows=N partitions=M"; NULL if none), for calibration.
PG_FUNCTION_INFO_V1(accel_postgres_get_last_maintenance_decision);
Datum accel_postgres_get_last_maintenance_decision(PG_FUNCTION_ARGS) {
  auto text = accelerator_codegen::aux_state::engine::postgres::aux_server::
      LastMaintenanceDecisionText();
  if (text.empty())
    text = accelerator_codegen::aux_state::core::FormatLastMaintenanceDecision();
  if (text.empty())
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(text.c_str()));
}

PG_FUNCTION_INFO_V1(accel_postgres_get_last_maintenance_action);
Datum accel_postgres_get_last_maintenance_action(PG_FUNCTION_ARGS) {
  const char *s = accel_pg_get_last_maintenance_action();
  if (!s || !*s)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(s));
}

} // extern "C"
