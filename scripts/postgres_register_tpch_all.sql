-- The generated deployment layer creates one statement-level trigger per
-- dependent table and operation. Transition tables are read-only relations:
--
--   AFTER INSERT ON t REFERENCING NEW TABLE AS new_rows
--   AFTER DELETE ON t REFERENCING OLD TABLE AS old_rows
--   AFTER UPDATE ON t REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
--
-- The trigger decoder maps those rows to MutationBatch before calling the
-- backend-local PostgresMutationCapture. Keeping trigger creation generated
-- avoids hard-coding table OIDs or query-specific column layouts here.

DROP FUNCTION IF EXISTS accel_aux_capture_transition CASCADE;
CREATE FUNCTION accel_aux_capture_transition()
RETURNS trigger
AS 'accelerator_codegen_pg', 'AcceleratorCodegenCaptureTransition'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_get_observer_mutation_count() CASCADE;
CREATE FUNCTION accel_postgres_get_observer_mutation_count()
RETURNS bigint
AS 'accelerator_codegen_pg', 'accel_postgres_get_observer_mutation_count'
LANGUAGE C STRICT;

DROP FUNCTION IF EXISTS accel_postgres_reset_observer() CASCADE;
CREATE FUNCTION accel_postgres_reset_observer()
RETURNS void
AS 'accelerator_codegen_pg', 'accel_postgres_reset_observer'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_set_force_maintenance_action(text) CASCADE;
CREATE FUNCTION accel_postgres_set_force_maintenance_action(text)
RETURNS void
AS 'accelerator_codegen_pg', 'accel_postgres_set_force_maintenance_action'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_get_last_maintenance_decision() CASCADE;
CREATE FUNCTION accel_postgres_get_last_maintenance_decision()
RETURNS text
AS 'accelerator_codegen_pg', 'accel_postgres_get_last_maintenance_decision'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_get_execute_counts() CASCADE;
CREATE FUNCTION accel_postgres_get_execute_counts()
RETURNS text
AS 'accelerator_codegen_pg', 'accel_postgres_get_execute_counts'
LANGUAGE C STRICT;

DROP FUNCTION IF EXISTS accel_postgres_get_last_maintenance_action() CASCADE;
CREATE FUNCTION accel_postgres_get_last_maintenance_action()
RETURNS text
AS 'accelerator_codegen_pg', 'accel_postgres_get_last_maintenance_action'
LANGUAGE C STRICT;

-- Shard workers: counters, and a stop switch.
DROP FUNCTION IF EXISTS accel_postgres_shard_stats() CASCADE;
CREATE FUNCTION accel_postgres_shard_stats()
RETURNS text
AS 'accelerator_codegen_pg', 'accel_postgres_shard_stats'
LANGUAGE C;

DROP FUNCTION IF EXISTS accel_postgres_stop_shard_workers() CASCADE;
CREATE FUNCTION accel_postgres_stop_shard_workers()
RETURNS boolean
AS 'accelerator_codegen_pg', 'accel_postgres_stop_shard_workers'
LANGUAGE C;

-- Per-query functions (accel_<bench>_qN, accel_<bench>_qN_aux, row types).
-- Their CREATE FUNCTION text lives with each query, in
-- accelerators/<bench>/q<N>/postgres/extension/functions.cpp, and is
-- registered in the module by a static registrar; this script names no query.
DROP FUNCTION IF EXISTS accel_postgres_register_queries() CASCADE;
CREATE FUNCTION accel_postgres_register_queries()
RETURNS integer
AS 'accelerator_codegen_pg', 'accel_postgres_register_queries'
LANGUAGE C STRICT;

SELECT accel_postgres_register_queries();
