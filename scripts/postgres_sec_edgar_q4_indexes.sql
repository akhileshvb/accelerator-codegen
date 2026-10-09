-- Optional partial indexes for the SEC-EDGAR Q4 accelerator on PostgreSQL.
--
-- The accelerator's initial build is one full join (parallel seq scans, no index
-- needed).  A structural write (sub / tag / pre) re-reads only the affected
-- (adsh, tag, version) keys from pre and num; with these indexes that is a few
-- index probes instead of a scan of num (tens of millions of rows) and pre.
-- Without them everything stays correct, structural writes are just slower.
-- The indexes cover only the rows Q4 can see (uom = 'USD', stmt = 'EQ'), so
-- they are small and plain writes to other rows do not touch them.
CREATE INDEX IF NOT EXISTS accel_q4_num_key ON num (adsh, tag, version)
    WHERE uom = 'USD' AND value IS NOT NULL;
CREATE INDEX IF NOT EXISTS accel_q4_pre_adsh ON pre (adsh, tag, version)
    WHERE stmt = 'EQ' AND adsh IS NOT NULL AND tag IS NOT NULL AND version IS NOT NULL;
CREATE INDEX IF NOT EXISTS accel_q4_pre_tag ON pre (tag, version)
    WHERE stmt = 'EQ' AND adsh IS NOT NULL AND tag IS NOT NULL AND version IS NOT NULL;
ANALYZE num;
ANALYZE pre;
