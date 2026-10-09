\ir postgres_accel_mutation_triggers.sql

-- SEC-EDGAR table ids (same as the DuckDB bindings): num=1 sub=2 tag=3 pre=4.
-- Idempotent: the same trigger names are reused by every SEC-EDGAR query.
SELECT create_accel_mutation_triggers('num', '1', 'accel_codegen_sec_edgar');
SELECT create_accel_mutation_triggers('sub', '2', 'accel_codegen_sec_edgar');
SELECT create_accel_mutation_triggers('tag', '3', 'accel_codegen_sec_edgar');
SELECT create_accel_mutation_triggers('pre', '4', 'accel_codegen_sec_edgar');
