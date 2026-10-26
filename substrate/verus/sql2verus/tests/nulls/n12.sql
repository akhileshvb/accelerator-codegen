WITH c AS (SELECT s, SUM(a) AS sa FROM t GROUP BY s) SELECT c.s, c.sa FROM c ORDER BY c.sa DESC, c.s
