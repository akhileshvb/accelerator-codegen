SELECT MAX(t.total) AS top FROM (SELECT store, SUM(amount) AS total FROM sales GROUP BY store) t
