SELECT store FROM sales GROUP BY store HAVING SUM(amount) + 1 > 5
