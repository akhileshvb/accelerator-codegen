-- the pre rows of the earlier filing arrive in a later batch
INSERT INTO pre (adsh, report, line, stmt, tag, version, plabel) VALUES ('t-2', 1, 1, 'IS', 'Pow2', 'v', 'Late');
DELETE FROM pre WHERE adsh = 't-1';
