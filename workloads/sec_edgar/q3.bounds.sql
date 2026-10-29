-- Bounds of q3.sql for the harness (eval/harness/float_bounds.hpp).  A group's SUM is rounded (n - 1
-- additions).  The HAVING threshold is the AVG of the per-cik sums, themselves rounded: each cik sum is
-- within d of its exact value e, and the AVG of those chosen numbers adds K rounded operations
-- (K - 1 additions and the division) on their absolute values.  A group may be in the answer if its
-- upper end exceeds the lowest threshold, and must be if its lower end exceeds the highest.
WITH subs AS (
    SELECT fsum(n2.value) AS e, fl_delta(COUNT(*) - 1, SUM(ABS(n2.value))) AS d
    FROM num n2
    JOIN sub s2 ON n2.adsh = s2.adsh
    WHERE n2.uom = $2 AND s2.fy = $1 AND n2.value IS NOT NULL
    GROUP BY s2.cik
), thr AS (
    SELECT fsum(e) / COUNT(*) - fsum(d) / COUNT(*) - fl_delta(COUNT(*), SUM(ABS(e) + d)) / COUNT(*) AS lo,
           fsum(e) / COUNT(*) + fsum(d) / COUNT(*) + fl_delta(COUNT(*), SUM(ABS(e) + d)) / COUNT(*) AS hi
    FROM subs
)
SELECT s.name, s.cik, fsum(n.value) AS total_value,
       fsum(n.value) - fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__lo,
       fsum(n.value) + fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) AS total_value__hi,
       fsum(n.value) + fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) > thr.lo AS __may,
       fsum(n.value) - fl_delta(COUNT(*) - 1, SUM(ABS(n.value))) > thr.hi AS __must
FROM num n
JOIN sub s ON n.adsh = s.adsh, thr
WHERE n.uom = $2 AND s.fy = $1 AND n.value IS NOT NULL
GROUP BY s.name, s.cik, thr.lo, thr.hi
