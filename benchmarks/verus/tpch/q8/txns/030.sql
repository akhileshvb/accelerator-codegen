UPDATE lineitem SET l_extendedprice = l_extendedprice + 12.34, l_discount = 0.05
WHERE l_orderkey IN (
  SELECT DISTINCT o_orderkey FROM orders
  JOIN lineitem ON l_orderkey = o_orderkey
  JOIN part ON p_partkey = l_partkey
  WHERE o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
    AND p_type IN ('LARGE STEEL', 'PROMO STEEL')
  ORDER BY o_orderkey LIMIT 64
);
UPDATE orders SET o_orderdate = DATE '1996-07-01' WHERE o_comment IN ('q8-order-03', 'q8-order-05');
