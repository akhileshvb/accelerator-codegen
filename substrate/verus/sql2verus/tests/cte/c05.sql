-- a CTE read in an IN subquery and in FROM
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust)
SELECT c_name FROM cust WHERE c_id IN (SELECT t_cust FROM tot WHERE t_amt > 100) AND c_id NOT IN (SELECT r_cust FROM returns) ORDER BY c_name
