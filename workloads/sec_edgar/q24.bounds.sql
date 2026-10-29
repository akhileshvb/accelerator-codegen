-- Bounds of q24.sql for the harness (eval/harness/float_bounds.hpp): per candidate group the interval of
-- the rounded SUM (n - 1 additions) and whether the group is in the answer.  HAVING is exact (COUNT).
SELECT n.tag, n.version, COUNT(*) AS cnt, fsum(n.value) AS total,
       fsum(n.value) - fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total__lo,
       fsum(n.value) + fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total__hi,
       true AS __may, true AS __must
FROM num n
LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh
WHERE n.uom = $2 AND n.ddate BETWEEN $1*10000+101 AND $1*10000+1231
  AND n.value IS NOT NULL
  AND p.adsh IS NULL
GROUP BY n.tag, n.version
HAVING COUNT(*) > 10
