SELECT region, store, SUM(amount) AS total
FROM sales
GROUP BY region, store
HAVING SUM(amount) > (SELECT AVG(sub_total) FROM (SELECT SUM(amount) AS sub_total FROM sales GROUP BY store) avg_sub)
ORDER BY total DESC
LIMIT 10
