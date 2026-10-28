-- one CTE, two aliases in the same FROM
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust)
SELECT a.t_cust, a.t_amt, b.t_amt AS other FROM tot a, tot b WHERE a.t_cust < b.t_cust AND a.t_amt = b.t_amt ORDER BY a.t_cust, b.t_cust
