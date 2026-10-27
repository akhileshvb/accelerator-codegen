SELECT store, AVG(amount) AS mean FROM sales GROUP BY store HAVING AVG(amount) >= (SELECT AVG(amount) FROM sales) ORDER BY store
