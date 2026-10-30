SELECT store, SUM(amount) AS s, AVG(amount) AS a FROM sales GROUP BY store ORDER BY store
