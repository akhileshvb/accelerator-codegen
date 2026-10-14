-- TPC-H Q17 (catalog text), :brand as $1, :container as $2.
SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem, part,
     (SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
      FROM lineitem GROUP BY l_partkey) part_agg
WHERE p_partkey = l_partkey AND agg_partkey = l_partkey
  AND p_brand = $1 AND p_container = $2
  AND l_quantity < avg_quantity
