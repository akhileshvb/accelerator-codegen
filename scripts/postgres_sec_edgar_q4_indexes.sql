-- SEC-EDGAR Q4 accelerator on PostgreSQL: re-read indexes.
--
-- The state keeps every (sic, tlabel, stmt, uom) group, so a structural write
-- (sub / tag / pre) re-reads pre and num rows for ANY stmt and uom, restricted
-- to the dirty (adsh, tag, version) keys.  The former partial indexes
-- (WHERE stmt = 'EQ' AND uom = 'USD') can no longer serve that re-read.
-- The full (adsh, tag, version) indexes created by postgres_sec_edgar_setup.sql
-- (num_adsh_tag_version_idx, pre_adsh_tag_version_idx) and the (tag, version)
-- index on pre (accel_q4_pre_tag, also created there) are sufficient, so this
-- script only makes sure they exist and drops the obsolete partial indexes.
DROP INDEX IF EXISTS accel_q4_num_key;
DROP INDEX IF EXISTS accel_q4_pre_adsh;
DROP INDEX IF EXISTS accel_q4_pre_tag; -- partial (stmt = 'EQ') variant, if present
CREATE INDEX IF NOT EXISTS num_adsh_tag_version_idx ON num (adsh, tag, version);
CREATE INDEX IF NOT EXISTS pre_adsh_tag_version_idx ON pre (adsh, tag, version);
CREATE INDEX IF NOT EXISTS accel_q4_pre_tag ON pre (tag, version);
ANALYZE num;
ANALYZE pre;
