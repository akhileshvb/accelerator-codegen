SELECT
  s_name,
  s_address
FROM
  supplier,
  nation
WHERE
  s_suppkey IN (
    SELECT
      ps_suppkey
    FROM
      partsupp,
(
        SELECT
          l_partkey agg_partkey,
          l_suppkey agg_suppkey,
          0.5 * SUM(l_quantity) AS agg_quantity
        FROM
          lineitem
        WHERE
          l_shipdate >= DATE '1995-01-01'
          AND l_shipdate < DATE '1995-01-01' + interval '1' year
        GROUP BY
          l_partkey,
          l_suppkey) agg_lineitem
      WHERE
        agg_partkey = ps_partkey
        AND agg_suppkey = ps_suppkey
        AND ps_partkey IN (
          SELECT
            p_partkey
          FROM
            part
          WHERE
            p_name LIKE 'violet%')
          AND ps_availqty > agg_quantity)
      AND s_nationkey = n_nationkey
      AND n_name = 'ARGENTINA'
    ORDER BY
      s_name;

