SELECT t.store, t.total FROM (SELECT store, SUM(amount) AS total FROM sales GROUP BY store) t ORDER BY t.store
