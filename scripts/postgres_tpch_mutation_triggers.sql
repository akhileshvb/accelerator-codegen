\ir postgres_accel_mutation_triggers.sql

-- TPC-H table ids (accelerators/tpch/common/postgres_tpch_table_ids.hpp):
-- lineitem=1 orders=2 customer=3 part=4 partsupp=5 supplier=6 nation=7 region=8
-- One statement-level trigger set per table. The id is the trigger argument.
SELECT create_accel_mutation_triggers('lineitem', '1', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('orders', '2', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('customer', '3', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('part', '4', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('partsupp', '5', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('supplier', '6', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('nation', '7', 'accel_codegen_tpch');
SELECT create_accel_mutation_triggers('region', '8', 'accel_codegen_tpch');
