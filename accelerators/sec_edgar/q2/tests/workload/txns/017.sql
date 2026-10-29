-- Mass withdrawal: the filings holding the top 1100 USD rows of fiscal year 2022 are deleted, draining the stored prefix of that class.
CREATE TEMP TABLE gone AS
SELECT DISTINCT adsh FROM (
  SELECT n.adsh FROM num n JOIN sub s ON n.adsh = s.adsh
  JOIN (SELECT adsh, tag, MAX(value) AS mv FROM num WHERE uom = 'USD' AND value IS NOT NULL GROUP BY adsh, tag) m
    ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.mv
  WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
  ORDER BY n.value DESC, s.name, n.tag LIMIT 1100);
DELETE FROM num WHERE adsh IN (SELECT adsh FROM gone);
DELETE FROM sub WHERE adsh IN (SELECT adsh FROM gone);
DROP TABLE gone;
