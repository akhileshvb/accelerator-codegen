\ir postgres_accel_mutation_triggers.sql

-- SEC-EDGAR table ids used by the Postgres query modules: sub=1 num=2.
-- Idempotent: the same trigger names are shared by every SEC-EDGAR query.
SELECT create_accel_mutation_triggers('sub', '1', 'accel_codegen_sec_edgar');
SELECT create_accel_mutation_triggers('num', '2', 'accel_codegen_sec_edgar');
