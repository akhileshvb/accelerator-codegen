SELECT store, SUM(amount) AS total FROM sales GROUP BY store HAVING SUM(amount) > 100 ORDER BY store
