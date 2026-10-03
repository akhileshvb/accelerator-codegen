-- TPC-H Q11 (catalog text), :nation_name as $1 (no LIMIT, as in the catalog). The fraction (0.0001) stays a literal: sql2verus cannot type a parameter used as a multiplier.
SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
FROM partsupp, supplier, nation
WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
  AND n_name = $1
GROUP BY ps_partkey
HAVING SUM(ps_supplycost * ps_availqty) > (SELECT SUM(ps_supplycost * ps_availqty) * 0.0001
                                           FROM partsupp, supplier, nation
                                           WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
                                             AND n_name = $1)
ORDER BY value DESC
