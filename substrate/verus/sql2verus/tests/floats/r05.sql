SELECT store, SUM(amount) / COUNT(*) AS mean FROM sales GROUP BY store
