-- Answer grain: Top-100 parts by value per nation (+ nation_total for threshold).
-- Note: official TPC-H fraction is 0.0001/SF; SEED1 uses 0.0001 (SF1).
-- ≤25 × 100 = 2500 rows (not all partkeys).
CREATE MATERIALIZED VIEW accel_tpch_q11_matview AS
WITH part_value AS (
  SELECT
    n.n_name,
    ps.ps_partkey,
    SUM(ps.ps_supplycost * ps.ps_availqty) AS value
  FROM partsupp ps
  JOIN supplier s ON ps.ps_suppkey = s.s_suppkey
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  GROUP BY n.n_name, ps.ps_partkey
),
nation_total AS (
  SELECT n_name, SUM(value) AS nation_total
  FROM part_value
  GROUP BY n_name
),
ranked AS (
  SELECT
    pv.n_name,
    pv.ps_partkey,
    pv.value,
    nt.nation_total,
    ROW_NUMBER() OVER (
      PARTITION BY pv.n_name ORDER BY pv.value DESC, pv.ps_partkey
    ) AS rn
  FROM part_value pv
  JOIN nation_total nt ON nt.n_name = pv.n_name
)
SELECT n_name, ps_partkey, value, nation_total
FROM ranked
WHERE rn <= 100;
