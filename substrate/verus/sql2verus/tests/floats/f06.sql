WITH t AS (SELECT store, SUM(amount) AS total FROM sales GROUP BY store)
SELECT store, total FROM t WHERE total > 100 ORDER BY store
