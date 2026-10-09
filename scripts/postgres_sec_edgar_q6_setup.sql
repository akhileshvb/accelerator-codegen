-- SEC EDGAR Q6 on PostgreSQL: one-time database preparation.
--  * C collation on the ORDER BY text keys (the accelerator orders byte-wise and
--    refuses to run otherwise);
--  * adsh indexes used by the incremental per-adsh maintenance lookups
--    (accelerator_codegen reads num / pre rows of the touched adsh).
ALTER TABLE sub ALTER COLUMN name TYPE VARCHAR(150) COLLATE "C";
ALTER TABLE num ALTER COLUMN tag TYPE VARCHAR(256) COLLATE "C";
ALTER TABLE pre ALTER COLUMN plabel TYPE VARCHAR(1024) COLLATE "C";
CREATE INDEX IF NOT EXISTS num_adsh_idx ON num (adsh);
CREATE INDEX IF NOT EXISTS pre_adsh_idx ON pre (adsh);
ANALYZE sub; ANALYZE num; ANALYZE pre;
