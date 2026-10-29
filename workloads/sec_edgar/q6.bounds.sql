-- Bounds of q6.sql for the harness (eval/harness/float_bounds.hpp): per group the interval of the rounded
-- SUM (n - 1 additions).  No HAVING: every group is in the answer.
SELECT s.name, p.stmt, n.tag, p.plabel,
       fsum(n.value) AS total_value,
       fsum(n.value) - fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__lo,
       fsum(n.value) + fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__hi,
       COUNT(*) AS cnt, true AS __may, true AS __must
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
WHERE n.uom = $3 AND p.stmt = $2 AND s.fy = $1
  AND n.value IS NOT NULL
GROUP BY s.name, p.stmt, n.tag, p.plabel
