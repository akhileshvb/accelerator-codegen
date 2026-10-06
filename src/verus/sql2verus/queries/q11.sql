-- TPC-H Q11 (catalog text), :nation_name as $1, :fraction (0.0001/SF) as $2 (no LIMIT, as in the catalog). $2 multiplies a sum, so sql2verus types it DECIMAL with 9 places (value * 10^9): exact for any fraction with at most 9 decimals (0.0001/SF for SF up to 100000).
SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
FROM partsupp, supplier, nation
WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
  AND n_name = $1
GROUP BY ps_partkey
HAVING SUM(ps_supplycost * ps_availqty) > (SELECT SUM(ps_supplycost * ps_availqty) * $2
                                           FROM partsupp, supplier, nation
                                           WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey
                                             AND n_name = $1)
ORDER BY value DESC
