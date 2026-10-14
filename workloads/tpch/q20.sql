-- TPC-H Q20 (catalog text), :start_date as $1, :part_name_pattern (:color || '%', e.g. 'gainsboro%') as $2, :nation_name as $3, $4 = $1 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT s_name, s_address
FROM supplier, nation
WHERE s_suppkey IN (SELECT ps_suppkey FROM partsupp,
                      (SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity
                       FROM lineitem WHERE l_shipdate >= $1
                         AND l_shipdate < $4
                       GROUP BY l_partkey, l_suppkey) agg_lineitem
                    WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey
                      AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE $2)
                      AND ps_availqty > agg_quantity)
  AND s_nationkey = n_nationkey AND n_name = $3
ORDER BY s_name
