-- Bounds of q4.sql for the harness (eval/harness/float_bounds.hpp): per group the intervals of the rounded
-- SUM (n - 1 additions) and AVG (the sum over n, so n rounded operations; its division may also
-- underflow: half the smallest subnormal).  HAVING is exact (COUNT DISTINCT).
SELECT s.sic, t.tlabel, p.stmt, COUNT(DISTINCT s.cik) AS num_companies,
       fsum(n.value) AS total_value,
       fsum(n.value) - fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__lo,
       fsum(n.value) + fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__hi,
       fsum(n.value) / COUNT(*) AS avg_value,
       fsum(n.value) / COUNT(*) - fl_delta(COUNT(*), SUM(ABS(n.value))) / COUNT(*) - 4.9e-324 AS avg_value__lo,
       fsum(n.value) / COUNT(*) + fl_delta(COUNT(*), SUM(ABS(n.value))) / COUNT(*) + 4.9e-324 AS avg_value__hi,
       true AS __may, true AS __must
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN tag t ON n.tag = t.tag AND n.version = t.version
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
WHERE n.uom = $4 AND p.stmt = $3
  AND s.sic BETWEEN $1 AND $2
  AND n.value IS NOT NULL AND t.abstract = 0
GROUP BY s.sic, t.tlabel, p.stmt
HAVING COUNT(DISTINCT s.cik) >= 2
