// Live PostgreSQL extension entrypoint.
// TPC-H SRFs live in query_functions.cpp / runtime.cpp.
// Statement-level mutation capture lives in transition_capture.cpp.

extern "C" {
#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>
PG_MODULE_MAGIC;
}

extern "C" {
void accel_pg_init_backend(const char *backing_dir);
int accel_pg_get_observer_mutation_count();
void accel_pg_reset_observer();
void accel_pg_set_force_maintenance_action(const char *action_name);
const char *accel_pg_get_last_maintenance_action(void);
}

namespace {
bool s_initialized = false;
void EnsureInit() {
  if (!s_initialized) {
    accel_pg_init_backend(nullptr);
    s_initialized = true;
  }
}
} // namespace

extern "C" {

PG_FUNCTION_INFO_V1(accel_postgres_get_observer_mutation_count);
Datum accel_postgres_get_observer_mutation_count(PG_FUNCTION_ARGS) {
  EnsureInit();
  PG_RETURN_INT64(accel_pg_get_observer_mutation_count());
}

PG_FUNCTION_INFO_V1(accel_postgres_reset_observer);
Datum accel_postgres_reset_observer(PG_FUNCTION_ARGS) {
  EnsureInit();
  accel_pg_reset_observer();
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(accel_postgres_set_force_maintenance_action);
Datum accel_postgres_set_force_maintenance_action(PG_FUNCTION_ARGS) {
  EnsureInit();
  if (PG_ARGISNULL(0)) {
    accel_pg_set_force_maintenance_action("");
  } else {
    char *cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
    accel_pg_set_force_maintenance_action(cstr);
    pfree(cstr);
  }
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(accel_postgres_get_last_maintenance_action);
Datum accel_postgres_get_last_maintenance_action(PG_FUNCTION_ARGS) {
  EnsureInit();
  const char *s = accel_pg_get_last_maintenance_action();
  if (!s || !*s)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(s));
}

void _PG_init(void) { EnsureInit(); }

} // extern "C"
