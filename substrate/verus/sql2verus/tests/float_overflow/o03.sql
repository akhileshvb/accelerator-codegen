SELECT store, COUNT(*) AS n FROM sales GROUP BY store HAVING SUM(amount) > (SELECT AVG(amount) FROM sales) ORDER BY store
