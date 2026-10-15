\ir postgres_accel_mutation_triggers.sql

-- One statement-level trigger set per table.
SELECT create_accel_mutation_triggers('lineitem', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('orders', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('customer', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('part', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('partsupp', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('supplier', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('nation', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('region', 'accel_codegen_tpch');
