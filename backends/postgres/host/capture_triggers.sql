-- Statement-level transition triggers for auxiliary-state mutation capture.
-- Generic: the capture function identifies the table by its oid and the
-- accelerators name their tables (PostgresAccelerator::Tables()), so a trigger
-- needs no workload-specific id. Workload scripts (e.g.
-- postgres_tpch_mutation_triggers.sql) call this per table.

CREATE OR REPLACE FUNCTION create_accel_mutation_triggers(
    table_name text,
    prefix text DEFAULT 'accel_codegen'
) RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    trigger_prefix text := prefix || '_' || table_name;
BEGIN
    IF to_regclass(table_name) IS NULL THEN
        RAISE NOTICE 'skipping triggers for missing table: %', table_name;
        RETURN;
    END IF;

    EXECUTE format(
        'DROP TRIGGER IF EXISTS %I ON %I',
        trigger_prefix || '_i', table_name
    );
    EXECUTE format(
        'DROP TRIGGER IF EXISTS %I ON %I',
        trigger_prefix || '_d', table_name
    );
    EXECUTE format(
        'DROP TRIGGER IF EXISTS %I ON %I',
        trigger_prefix || '_u', table_name
    );
    EXECUTE format(
        'DROP TRIGGER IF EXISTS %I ON %I',
        trigger_prefix || '_t', table_name
    );

    EXECUTE format(
        'CREATE TRIGGER %I AFTER INSERT ON %I '
        'REFERENCING NEW TABLE AS new_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition()',
        trigger_prefix || '_i', table_name
    );
    EXECUTE format(
        'CREATE TRIGGER %I AFTER DELETE ON %I '
        'REFERENCING OLD TABLE AS old_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition()',
        trigger_prefix || '_d', table_name
    );
    EXECUTE format(
        'CREATE TRIGGER %I AFTER UPDATE ON %I '
        'REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition()',
        trigger_prefix || '_u', table_name
    );
    -- TRUNCATE has no transition tables; the capture function only announces
    -- the write (cross-backend sync) and forces a resync of this backend.
    EXECUTE format(
        'CREATE TRIGGER %I AFTER TRUNCATE ON %I '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition()',
        trigger_prefix || '_t', table_name
    );
END;
$$;
