SELECT store, SUM(bonus) AS b FROM sales GROUP BY store HAVING SUM(bonus) > 10 ORDER BY store
