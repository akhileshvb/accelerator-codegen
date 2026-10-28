-- a CTE read in both branches of a UNION ALL
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust)
SELECT t_cust, t_amt FROM tot WHERE t_amt > 100
UNION ALL
SELECT t_cust, t_amt FROM tot WHERE t_amt < 10
