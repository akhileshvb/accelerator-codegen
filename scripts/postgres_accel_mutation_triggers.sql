-- Statement-level transition triggers for auxiliary-state mutation capture.
-- Table IDs match postgres_runtime.cpp SecEdgar/Tpch configs.

CREATE OR REPLACE FUNCTION create_accel_mutation_triggers(
    table_name text,
    table_id text,
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
        'CREATE TRIGGER %I AFTER INSERT ON %I '
        'REFERENCING NEW TABLE AS new_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition(%L)',
        trigger_prefix || '_i', table_name, table_id
    );
    EXECUTE format(
        'CREATE TRIGGER %I AFTER DELETE ON %I '
        'REFERENCING OLD TABLE AS old_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition(%L)',
        trigger_prefix || '_d', table_name, table_id
    );
    EXECUTE format(
        'CREATE TRIGGER %I AFTER UPDATE ON %I '
        'REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows '
        'FOR EACH STATEMENT '
        'EXECUTE FUNCTION accel_aux_capture_transition(%L)',
        trigger_prefix || '_u', table_name, table_id
    );
END;
$$;

-- Remove legacy generic num triggers before reinstalling the single shared capture.
DROP TRIGGER IF EXISTS accel_codegen_sec_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_num_d ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q2_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q2_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q2_num_d ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q3_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q3_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q3_num_d ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q4_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q4_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q4_num_d ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q6_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q6_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q6_num_d ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q24_num_i ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q24_num_u ON num;
DROP TRIGGER IF EXISTS accel_codegen_sec_q24_num_d ON num;

-- SEC EDGAR logical table IDs: sub=101, pre=102, num=103, tag=104.
SELECT create_accel_mutation_triggers('sub', '101', 'accel_codegen_sec');
SELECT create_accel_mutation_triggers('pre', '102', 'accel_codegen_sec');
SELECT create_accel_mutation_triggers('num', '103', 'accel_codegen_sec');
SELECT create_accel_mutation_triggers('tag', '104', 'accel_codegen_sec');

-- TPC-H canonical table ids (postgres_tpch_table_ids.hpp):
-- lineitem=1 orders=2 customer=3 part=4 partsupp=5 supplier=6 nation=7 region=8
-- One statement-level trigger set per table. Capture prefers relname over args.
SELECT create_accel_mutation_triggers('lineitem', '1', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('orders', '2', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('customer', '3', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('part', '4', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('partsupp', '5', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('supplier', '6', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('nation', '7', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('region', '8', 'accel_codegen_tpch');
