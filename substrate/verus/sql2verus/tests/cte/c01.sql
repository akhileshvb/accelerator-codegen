-- a CTE referenced from FROM and from a correlated subquery
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust)
SELECT c_name FROM tot t1, cust WHERE t1.t_cust = c_id AND t1.t_amt > (SELECT AVG(t2.t_amt) * 1.2 FROM tot t2 WHERE t2.t_cust = t1.t_cust) ORDER BY c_name
