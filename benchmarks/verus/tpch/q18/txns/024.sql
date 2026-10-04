UPDATE lineitem SET l_quantity = l_quantity + 2.25
WHERE l_orderkey IN (
  SELECT o_orderkey FROM orders
  JOIN customer ON c_custkey = o_custkey
  JOIN lineitem ON l_orderkey = o_orderkey
  WHERE o_comment NOT LIKE 'q18:%'
  GROUP BY o_orderkey HAVING SUM(l_quantity) > 180
  ORDER BY o_orderkey LIMIT 20
);
