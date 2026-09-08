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

-- TPC-H logical table IDs: customer=1, nation=2, orders=3, lineitem=4, part=5.
SELECT create_accel_mutation_triggers('customer', '1', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('nation', '2', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('orders', '3', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('lineitem', '4', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('lineitem', '1001', 'accel_codegen_tpch_q1');
SELECT create_accel_mutation_triggers('orders', '1002', 'accel_codegen_tpch_q3_orders');
SELECT create_accel_mutation_triggers('lineitem', '1004', 'accel_codegen_tpch_q3');
SELECT create_accel_mutation_triggers('lineitem', '1006', 'accel_codegen_tpch_q6');
SELECT create_accel_mutation_triggers('orders', '1012', 'accel_codegen_tpch_q12_orders');
SELECT create_accel_mutation_triggers('lineitem', '1013', 'accel_codegen_tpch_q12');
SELECT create_accel_mutation_triggers('lineitem', '1014', 'accel_codegen_tpch_q14');
SELECT create_accel_mutation_triggers('part', '5', 'accel_codegen_tpch');

-- Per-query projections for remaining TPC-H aux kernels.
SELECT create_accel_mutation_triggers('part', '1091', 'accel_codegen_tpch_q9');
SELECT create_accel_mutation_triggers('supplier', '1092', 'accel_codegen_tpch_q9');
SELECT create_accel_mutation_triggers('nation', '1093', 'accel_codegen_tpch_q9');
SELECT create_accel_mutation_triggers('orders', '1094', 'accel_codegen_tpch_q9');
SELECT create_accel_mutation_triggers('partsupp', '1095', 'accel_codegen_tpch_q9');
SELECT create_accel_mutation_triggers('lineitem', '1096', 'accel_codegen_tpch_q9');

SELECT create_accel_mutation_triggers('partsupp', '1111', 'accel_codegen_tpch_q11');
SELECT create_accel_mutation_triggers('supplier', '1112', 'accel_codegen_tpch_q11');
SELECT create_accel_mutation_triggers('nation', '1113', 'accel_codegen_tpch_q11');

SELECT create_accel_mutation_triggers('part', '1171', 'accel_codegen_tpch_q17');
SELECT create_accel_mutation_triggers('lineitem', '1172', 'accel_codegen_tpch_q17');

SELECT create_accel_mutation_triggers('lineitem', '1180', 'accel_codegen_tpch_q18');
SELECT create_accel_mutation_triggers('orders', '1181', 'accel_codegen_tpch_q18');
SELECT create_accel_mutation_triggers('customer', '1182', 'accel_codegen_tpch_q18');

SELECT create_accel_mutation_triggers('supplier', '1211', 'accel_codegen_tpch_q21');
SELECT create_accel_mutation_triggers('orders', '1212', 'accel_codegen_tpch_q21');
SELECT create_accel_mutation_triggers('lineitem', '1213', 'accel_codegen_tpch_q21');
SELECT create_accel_mutation_triggers('nation', '1214', 'accel_codegen_tpch_q21');

SELECT create_accel_mutation_triggers('orders', '1221', 'accel_codegen_tpch_q22');
SELECT create_accel_mutation_triggers('customer', '1222', 'accel_codegen_tpch_q22');

SELECT create_accel_mutation_triggers('part', '1021', 'accel_codegen_tpch_q2');
SELECT create_accel_mutation_triggers('supplier', '1022', 'accel_codegen_tpch_q2');
SELECT create_accel_mutation_triggers('partsupp', '1023', 'accel_codegen_tpch_q2');
SELECT create_accel_mutation_triggers('nation', '1024', 'accel_codegen_tpch_q2');
SELECT create_accel_mutation_triggers('region', '1025', 'accel_codegen_tpch_q2');

SELECT create_accel_mutation_triggers('orders', '1041', 'accel_codegen_tpch_q4');
SELECT create_accel_mutation_triggers('lineitem', '1042', 'accel_codegen_tpch_q4');

SELECT create_accel_mutation_triggers('region', '1051', 'accel_codegen_tpch_q5');
SELECT create_accel_mutation_triggers('nation', '1052', 'accel_codegen_tpch_q5');
SELECT create_accel_mutation_triggers('customer', '1053', 'accel_codegen_tpch_q5');
SELECT create_accel_mutation_triggers('supplier', '1054', 'accel_codegen_tpch_q5');
SELECT create_accel_mutation_triggers('orders', '1055', 'accel_codegen_tpch_q5');
SELECT create_accel_mutation_triggers('lineitem', '1056', 'accel_codegen_tpch_q5');

SELECT create_accel_mutation_triggers('nation', '1071', 'accel_codegen_tpch_q7');
SELECT create_accel_mutation_triggers('supplier', '1072', 'accel_codegen_tpch_q7');
SELECT create_accel_mutation_triggers('customer', '1073', 'accel_codegen_tpch_q7');
SELECT create_accel_mutation_triggers('orders', '1074', 'accel_codegen_tpch_q7');
SELECT create_accel_mutation_triggers('lineitem', '1075', 'accel_codegen_tpch_q7');

SELECT create_accel_mutation_triggers('part', '1081', 'accel_codegen_tpch_q8');
SELECT create_accel_mutation_triggers('supplier', '1082', 'accel_codegen_tpch_q8');
SELECT create_accel_mutation_triggers('customer', '1083', 'accel_codegen_tpch_q8');
SELECT create_accel_mutation_triggers('orders', '1084', 'accel_codegen_tpch_q8');
SELECT create_accel_mutation_triggers('lineitem', '1085', 'accel_codegen_tpch_q8');

SELECT create_accel_mutation_triggers('customer', '1131', 'accel_codegen_tpch_q13');
SELECT create_accel_mutation_triggers('orders', '1132', 'accel_codegen_tpch_q13');

SELECT create_accel_mutation_triggers('lineitem', '1151', 'accel_codegen_tpch_q15');
SELECT create_accel_mutation_triggers('supplier', '1152', 'accel_codegen_tpch_q15');

SELECT create_accel_mutation_triggers('part', '1161', 'accel_codegen_tpch_q16');
SELECT create_accel_mutation_triggers('partsupp', '1162', 'accel_codegen_tpch_q16');
SELECT create_accel_mutation_triggers('supplier', '1163', 'accel_codegen_tpch_q16');

SELECT create_accel_mutation_triggers('lineitem', '1191', 'accel_codegen_tpch_q19');
SELECT create_accel_mutation_triggers('part', '1192', 'accel_codegen_tpch_q19');

SELECT create_accel_mutation_triggers('lineitem', '1201', 'accel_codegen_tpch_q20');
SELECT create_accel_mutation_triggers('part', '1202', 'accel_codegen_tpch_q20');
SELECT create_accel_mutation_triggers('partsupp', '1203', 'accel_codegen_tpch_q20');
SELECT create_accel_mutation_triggers('supplier', '1204', 'accel_codegen_tpch_q20');
SELECT create_accel_mutation_triggers('nation', '1205', 'accel_codegen_tpch_q20');
