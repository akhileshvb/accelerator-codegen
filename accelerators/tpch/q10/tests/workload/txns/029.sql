UPDATE orders SET o_orderdate = DATE '1993-11-15'
WHERE o_orderkey = (
  SELECT o_orderkey FROM orders JOIN lineitem ON o_orderkey = l_orderkey
  WHERE o_orderdate >= DATE '1994-10-01' AND o_orderdate < DATE '1995-01-01'
    AND l_returnflag = 'R' AND o_comment NOT LIKE 'Q10%'
  GROUP BY o_orderkey ORDER BY sum(l_extendedprice * (1 - l_discount)) DESC, o_orderkey LIMIT 1
);
