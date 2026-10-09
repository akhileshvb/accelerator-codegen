-- One-time preparation of a SEC-EDGAR PostgreSQL database for the
-- accelerator_codegen SEC-EDGAR aux accelerators (q1, q2, q3, q4, q6, q24).
-- Run after loading benchmarks/sec_edgar/schema.sql data and before
-- CREATE EXTENSION / installing the capture triggers. Idempotent.
--
--  * C collation on ORDER BY text keys: the accelerators order byte-wise, so
--    plain SQL must too (q6 refuses to run otherwise).
--  * (adsh, tag, version) indexes on num and pre: incremental maintenance
--    (q4 structural re-reads, q6 per-adsh lookups, q24 pre DELETE / key UPDATE)
--    resolves touched keys with equality probes; without these each probe scans
--    num (tens of millions of rows). The adsh prefix also serves adsh-only
--    lookups, so separate adsh indexes are unnecessary.
--  * pre (tag, version) for q4's tag-keyed re-read, limited to the rows q4 sees.
--
-- Per-query scripts (postgres_sec_edgar_q4_indexes.sql,
-- postgres_sec_edgar_q24_indexes.sql, postgres_sec_edgar_q6_setup.sql) remain
-- for the per-query tests; this script is the superset for real databases.

ALTER TABLE sub ALTER COLUMN name TYPE VARCHAR(150) COLLATE "C";
ALTER TABLE num ALTER COLUMN tag TYPE VARCHAR(256) COLLATE "C";
ALTER TABLE pre ALTER COLUMN plabel TYPE VARCHAR(1024) COLLATE "C";

CREATE INDEX IF NOT EXISTS num_adsh_tag_version_idx ON num (adsh, tag, version);
CREATE INDEX IF NOT EXISTS pre_adsh_tag_version_idx ON pre (adsh, tag, version);
CREATE INDEX IF NOT EXISTS accel_q4_pre_tag ON pre (tag, version)
    WHERE stmt = 'EQ' AND adsh IS NOT NULL AND tag IS NOT NULL AND version IS NOT NULL;

ANALYZE sub;
ANALYZE num;
ANALYZE pre;
