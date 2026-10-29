-- Near-total withdrawal of a class: all but about 150 filings with CNY rows in fiscal year 2022 are deleted.
CREATE TEMP TABLE gone AS
SELECT adsh FROM (
  SELECT DISTINCT s.adsh FROM sub s JOIN num n ON n.adsh = s.adsh WHERE s.fy = 2022 AND n.uom = 'CNY'
) ORDER BY hash(adsh) OFFSET 150;
DELETE FROM num WHERE adsh IN (SELECT adsh FROM gone);
DELETE FROM sub WHERE adsh IN (SELECT adsh FROM gone);
DROP TABLE gone;
