SELECT m.s, m.mx FROM (SELECT s, MAX(a) AS mx FROM t GROUP BY s) m WHERE m.mx > 3 ORDER BY m.s
