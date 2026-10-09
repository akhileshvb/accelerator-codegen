\ir postgres_accel_mutation_triggers.sql

-- SEC EDGAR Q1 reads `pre` only. The id is the trigger argument and must match
-- kPreTableId in src/generated/sec_edgar/q1/postgres/extension/scan.cpp.
SELECT create_accel_mutation_triggers('pre', '3', 'accel_codegen_sec_edgar');
