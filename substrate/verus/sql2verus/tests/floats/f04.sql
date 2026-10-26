SELECT SUM(t.total) AS grand FROM (SELECT store, SUM(amount) AS total FROM sales GROUP BY store) t
