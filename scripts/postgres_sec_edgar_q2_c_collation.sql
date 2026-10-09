-- SEC-EDGAR Q2 orders its result by sub.name and num.tag byte-wise; the
-- accelerator refuses to run unless those columns use the C collation.
ALTER TABLE sub ALTER COLUMN name TYPE character varying(150) COLLATE "C";
ALTER TABLE num ALTER COLUMN tag TYPE character varying(256) COLLATE "C";
