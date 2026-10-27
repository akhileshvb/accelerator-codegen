-- a CTE over another CTE
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust),
     big AS (SELECT t_cust, t_amt FROM tot WHERE t_amt > 100)
SELECT c_name, t_amt FROM big, cust WHERE t_cust = c_id ORDER BY c_name
