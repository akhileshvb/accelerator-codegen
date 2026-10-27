-- a CTE whose body is a UNION ALL, read twice
WITH flow AS (SELECT o_cust AS f_cust, o_amt AS f_amt FROM orders UNION ALL SELECT r_cust, -r_amt FROM returns)
SELECT a.f_cust, SUM(a.f_amt) AS s FROM flow a, flow b WHERE a.f_cust = b.f_cust AND b.f_amt > 50 GROUP BY a.f_cust ORDER BY a.f_cust
