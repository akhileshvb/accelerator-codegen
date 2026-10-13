-- Answer grain: Top-100 waiting suppliers per nation (not all supplier names).
CREATE MATERIALIZED VIEW accel_tpch_q21_matview AS
WITH waits AS (
  SELECT
    n.n_name,
    s.s_name,
    COUNT(*)::bigint AS numwait
  FROM supplier s
  JOIN lineitem l1 ON s.s_suppkey = l1.l_suppkey
  JOIN orders o ON o.o_orderkey = l1.l_orderkey
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  WHERE o.o_orderstatus = 'F'
    AND l1.l_receiptdate > l1.l_commitdate
    AND EXISTS (
      SELECT 1 FROM lineitem l2
      WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey
    )
    AND NOT EXISTS (
      SELECT 1 FROM lineitem l3
      WHERE l3.l_orderkey = l1.l_orderkey
        AND l3.l_suppkey <> l1.l_suppkey
        AND l3.l_receiptdate > l3.l_commitdate
    )
  GROUP BY n.n_name, s.s_name
),
ranked AS (
  SELECT
    n_name, s_name, numwait,
    ROW_NUMBER() OVER (
      PARTITION BY n_name ORDER BY numwait DESC, s_name
    ) AS rn
  FROM waits
)
SELECT n_name, s_name, numwait
FROM ranked
WHERE rn <= 100;
