// GENERATED BACKEND - query-facing SQL entry points for Postgres.
// The accel_postgres_* functions declared in
// backends/postgres/engine/accelerator_codegen_postgres--0.1.sql (forced
// maintenance action, execute counts, last maintenance decision, shard worker
// stats and stop).
// Per-query SQL functions register through backends/postgres/api/runtime.cpp.
// Linked into the accelerator_codegen_pg MODULE alongside
// backends/postgres/engine/extension_engine.cpp (which has PG_MODULE_MAGIC).

#include <string>

#include "backends/postgres/state/shard_worker.hpp"
#include "substrate/aux/core/aux_execute_stats.hpp"

extern "C" {

#include <postgres.h>
#include <fmgr.h>
#include <utils/builtins.h>

PG_FUNCTION_INFO_V1(accel_postgres_set_force_maintenance_action);
Datum accel_postgres_set_force_maintenance_action(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0)) {
    accelerator_codegen::aux_state::engine::postgres::shard::SetForcedMaintenance("");
  } else {
    char *cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));
    accelerator_codegen::aux_state::engine::postgres::shard::SetForcedMaintenance(cstr);
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

// The last maintenance decision a shard worker made
// ("action=A rows=N partitions=M"; NULL if none), for calibration.
PG_FUNCTION_INFO_V1(accel_postgres_get_last_maintenance_decision);
Datum accel_postgres_get_last_maintenance_decision(PG_FUNCTION_ARGS) {
  const auto text = accelerator_codegen::aux_state::engine::postgres::shard::
      LastMaintenanceDecisionText();
  if (text.empty())
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(cstring_to_text(text.c_str()));
}

PG_FUNCTION_INFO_V1(accel_postgres_get_last_maintenance_action);
Datum accel_postgres_get_last_maintenance_action(PG_FUNCTION_ARGS) {
  const auto action = accelerator_codegen::aux_state::engine::postgres::shard::LastMaintenanceActionName();
  PG_RETURN_TEXT_P(cstring_to_text(action.empty() ? "None" : action.c_str()));
}

// Counters of the database's running shard workers ("requests=N served=N ...";
// "no worker" if none).
PG_FUNCTION_INFO_V1(accel_postgres_shard_stats);
Datum accel_postgres_shard_stats(PG_FUNCTION_ARGS) {
  const auto text = accelerator_codegen::aux_state::engine::postgres::shard::StatsText();
  PG_RETURN_TEXT_P(cstring_to_text(text.c_str()));
}

// Stops the database's shard workers (they restart on demand); false if none
// was running.
PG_FUNCTION_INFO_V1(accel_postgres_stop_shard_workers);
Datum accel_postgres_stop_shard_workers(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(accelerator_codegen::aux_state::engine::postgres::shard::StopWorkers());
}

} // extern "C"
