-- Withdrawal and re-filing in one transaction: the filing of the largest USD value of 2024 is deleted and filed again under a new accession number.
CREATE TEMP TABLE old AS SELECT adsh FROM (SELECT n.adsh FROM num n JOIN sub s ON n.adsh = s.adsh WHERE s.fy = 2024 AND n.uom = 'USD' AND n.value IS NOT NULL ORDER BY n.value DESC, n.adsh LIMIT 1);
INSERT INTO sub SELECT * REPLACE ('RE-' || adsh AS adsh) FROM sub WHERE adsh IN (SELECT adsh FROM old);
INSERT INTO num SELECT * REPLACE ('RE-' || adsh AS adsh) FROM num WHERE adsh IN (SELECT adsh FROM old);
DELETE FROM num WHERE adsh IN (SELECT adsh FROM old);
DELETE FROM sub WHERE adsh IN (SELECT adsh FROM old);
DROP TABLE old;
