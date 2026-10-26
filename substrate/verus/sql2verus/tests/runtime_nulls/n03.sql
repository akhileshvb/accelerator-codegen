SELECT b, SUM(c) AS s, MAX(d) AS m, MIN(ts) AS lo FROM t GROUP BY b ORDER BY b
