UPDATE orders SET o_comment = 'q18:delete:batch', o_totalprice = o_totalprice + 1.00
WHERE o_orderkey IN (
  SELECT o_orderkey FROM orders
  JOIN customer ON c_custkey = o_custkey
  JOIN lineitem ON l_orderkey = o_orderkey
  WHERE o_comment NOT LIKE 'q18:%'
  GROUP BY o_orderkey HAVING SUM(l_quantity) > 180
  ORDER BY o_orderkey LIMIT 5
);
DELETE FROM lineitem WHERE l_orderkey IN (
  SELECT o_orderkey FROM orders WHERE o_comment = 'q18:delete:batch'
);
DELETE FROM orders WHERE o_comment = 'q18:delete:batch';
