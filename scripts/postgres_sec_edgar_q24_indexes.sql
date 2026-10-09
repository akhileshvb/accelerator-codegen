-- Triple indexes for SEC-EDGAR Q24. The aux module's pre DELETE / key UPDATE
-- maintenance resolves each touched (adsh, tag, version) triple with equality
-- probes on both tables; with these indexes every probe is an index lookup.
CREATE INDEX IF NOT EXISTS num_adsh_tag_version_idx ON num (adsh, tag, version);
CREATE INDEX IF NOT EXISTS pre_adsh_tag_version_idx ON pre (adsh, tag, version);
ANALYZE num;
ANALYZE pre;
