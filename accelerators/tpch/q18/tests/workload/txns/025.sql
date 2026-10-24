UPDATE customer SET c_name = SUBSTRING(c_name, 1, 22) || ' II'
WHERE c_custkey IN (
  SELECT DISTINCT c_custkey FROM customer
  JOIN orders ON o_custkey = c_custkey
  JOIN (SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 180) AS eligible
    ON eligible.l_orderkey = orders.o_orderkey
  WHERE c_comment NOT LIKE 'q18:%'
  ORDER BY c_custkey LIMIT 10
);
