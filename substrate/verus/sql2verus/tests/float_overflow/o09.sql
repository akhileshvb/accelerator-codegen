SELECT store, SUM(amount * qty) AS s FROM sales GROUP BY store ORDER BY store
