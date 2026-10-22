SELECT
  SUM(l_extendedprice) / 7.0 AS avg_yearly
FROM
  lineitem,
  part,
(
    SELECT
      l_partkey AS agg_partkey,
      0.2 * AVG(l_quantity) AS avg_quantity
    FROM
      lineitem
    GROUP BY
      l_partkey) part_agg
WHERE
  p_partkey = l_partkey
  AND agg_partkey = l_partkey
  AND p_brand = 'Brand#44'
  AND p_container = 'ECONOMY ANODIZED'
  AND l_quantity < avg_quantity;

