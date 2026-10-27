WITH t AS (SELECT store, SUM(amount) AS total FROM sales GROUP BY store)
SELECT a.store AS lo, b.store AS hi FROM t a, t b WHERE a.total < b.total ORDER BY lo, hi
