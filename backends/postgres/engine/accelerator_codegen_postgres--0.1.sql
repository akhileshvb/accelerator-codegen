-- The generated deployment layer creates one statement-level trigger per
-- dependent table and operation. Transition tables are read-only relations:
--
--   AFTER INSERT ON t REFERENCING NEW TABLE AS new_rows
--   AFTER DELETE ON t REFERENCING OLD TABLE AS old_rows
--   AFTER UPDATE ON t REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
--
-- The capture function (transition_capture.cpp) reads those rows into one
-- columnar batch per statement and stages it with the transaction. Keeping
-- trigger creation generated avoids hard-coding table OIDs or query-specific
-- column layouts here.

CREATE FUNCTION accel_aux_capture_transition()
RETURNS trigger
AS 'MODULE_PATHNAME', 'AcceleratorCodegenCaptureTransition'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_observer_mutation_count()
RETURNS bigint
AS 'MODULE_PATHNAME', 'accel_postgres_get_observer_mutation_count'
LANGUAGE C STRICT;

CREATE FUNCTION accel_postgres_reset_observer()
RETURNS void
AS 'MODULE_PATHNAME', 'accel_postgres_reset_observer'
LANGUAGE C;

CREATE FUNCTION accel_postgres_set_force_maintenance_action(text)
RETURNS void
AS 'MODULE_PATHNAME', 'accel_postgres_set_force_maintenance_action'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_last_maintenance_decision()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_last_maintenance_decision'
LANGUAGE C;

CREATE FUNCTION accel_postgres_get_execute_counts()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_execute_counts'
LANGUAGE C STRICT;

CREATE FUNCTION accel_postgres_get_last_maintenance_action()
RETURNS text
AS 'MODULE_PATHNAME', 'accel_postgres_get_last_maintenance_action'
LANGUAGE C STRICT;

-- Per-query functions (accel_<bench>_qN, accel_<bench>_qN_aux, row types) are
-- not listed here.  Each query carries its own CREATE FUNCTION text in
-- src/generated/<bench>/q<N>/postgres/extension/functions.cpp, registered in
-- the module; this function runs all of them.
CREATE FUNCTION accel_postgres_register_queries()
RETURNS integer
AS 'MODULE_PATHNAME', 'accel_postgres_register_queries'
LANGUAGE C STRICT;

SELECT accel_postgres_register_queries();
