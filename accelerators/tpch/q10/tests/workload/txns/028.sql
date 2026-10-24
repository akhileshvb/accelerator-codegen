UPDATE customer SET c_acctbal = c_acctbal + 0.01
WHERE c_custkey = (
  SELECT c_custkey FROM customer JOIN orders ON c_custkey = o_custkey
  JOIN lineitem ON o_orderkey = l_orderkey JOIN nation ON c_nationkey = n_nationkey
  WHERE o_orderdate >= DATE '1993-10-01' AND o_orderdate < DATE '1994-01-01' AND l_returnflag = 'R'
  GROUP BY c_custkey ORDER BY sum(l_extendedprice * (1 - l_discount)) DESC, c_custkey LIMIT 1
);
