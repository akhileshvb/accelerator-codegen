-- Idempotent migration: give every TPC-H text column C collation, matching
-- scripts/postgres_tpch_schema.sql.  The verified accelerators order text
-- byte-wise; PostgreSQL does the same exactly under a C collation, and the
-- Q21 runtime refuses to run otherwise.  A collation-only change of a varchar
-- column needs no table rewrite (indexes on the column are rebuilt).
DO $$
DECLARE
  c record;
BEGIN
  FOR c IN
    SELECT a.attrelid::regclass AS rel, a.attname AS col
    FROM pg_attribute a
    JOIN pg_class t ON t.oid = a.attrelid
    JOIN pg_namespace ns ON ns.oid = t.relnamespace
    WHERE ns.nspname = current_schema()
      AND t.relname IN ('region', 'nation', 'customer', 'supplier', 'part',
                        'partsupp', 'orders', 'lineitem')
      AND a.attnum > 0 AND NOT a.attisdropped
      AND a.atttypid = 'character varying'::regtype
      AND a.attcollation <> '"C"'::regcollation
  LOOP
    EXECUTE format('ALTER TABLE %s ALTER COLUMN %I TYPE character varying COLLATE "C"',
                   c.rel, c.col);
    RAISE NOTICE 'collation C: %.%', c.rel, c.col;
  END LOOP;
END;
$$;
