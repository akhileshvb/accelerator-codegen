-- Answer grain: Top-100 by price per threshold ∈ {312,313,314,315}.
-- ≤400 rows (SF3000-safe); residual filters threshold.
CREATE MATERIALIZED VIEW accel_tpch_q18_matview AS
WITH thresholds(quantity_threshold) AS (
  VALUES (312::numeric), (313), (314), (315)
),
heavy AS (
  SELECT
    t.quantity_threshold,
    c.c_name,
    c.c_custkey,
    o.o_orderkey,
    o.o_orderdate,
    o.o_totalprice,
    oq.order_qty AS sum_qty,
    ROW_NUMBER() OVER (
      PARTITION BY t.quantity_threshold
      ORDER BY o.o_totalprice DESC, o.o_orderdate, o.o_orderkey
    ) AS rn
  FROM thresholds t
  JOIN (
    SELECT l_orderkey, SUM(l_quantity) AS order_qty
    FROM lineitem
    GROUP BY l_orderkey
  ) oq ON oq.order_qty > t.quantity_threshold
  JOIN orders o ON o.o_orderkey = oq.l_orderkey
  JOIN customer c ON c.c_custkey = o.o_custkey
)
SELECT
  quantity_threshold, c_name, c_custkey, o_orderkey,
  o_orderdate, o_totalprice, sum_qty
FROM heavy
WHERE rn <= 100;
