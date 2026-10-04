UPDATE orders SET o_totalprice = o_totalprice + 12.34,
                  o_orderdate = o_orderdate + 2
WHERE o_orderkey IN (
  SELECT orders.o_orderkey FROM orders
  JOIN customer ON c_custkey = o_custkey
  JOIN lineitem ON l_orderkey = o_orderkey
  WHERE o_comment NOT LIKE 'q18:%'
  GROUP BY orders.o_orderkey HAVING SUM(l_quantity) > 180
  ORDER BY orders.o_orderkey LIMIT 5
);
