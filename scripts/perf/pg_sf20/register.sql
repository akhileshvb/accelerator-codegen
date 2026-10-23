-- Register the TPC-H accelerators and the capture triggers (psql -v scripts=<repo>/scripts).
LOAD '/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so';
\i :scripts/../backends/postgres/host/register.sql
\i :scripts/postgres_tpch_mutation_triggers.sql
