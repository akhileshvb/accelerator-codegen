SELECT store, SUM(bonus) AS b FROM sales GROUP BY store HAVING SUM(bonus) > (SELECT AVG(s) FROM (SELECT SUM(bonus) AS s FROM sales GROUP BY store) t) ORDER BY store
