-- a CTE under an alias of an alias, used as the right side of a LEFT JOIN
WITH tot AS (SELECT o_cust AS t_cust, SUM(o_amt) AS t_amt FROM orders GROUP BY o_cust)
SELECT c_id, COUNT(x.t_cust) AS n FROM cust LEFT JOIN tot x ON x.t_cust = c_id GROUP BY c_id ORDER BY c_id
