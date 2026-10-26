SELECT s, SUM(a) AS sa, COUNT(a) AS ca, MIN(b) AS lo, MAX(b) AS hi, AVG(a) AS av FROM t GROUP BY s ORDER BY s
