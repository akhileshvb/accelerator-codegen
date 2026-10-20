-- Answer grain: Top-100 per (size∈1..50 × Syllable3 × region). ≤125k rows.
CREATE MATERIALIZED VIEW accel_tpch_q2_matview AS
WITH regional_min AS (
  SELECT ps.ps_partkey, r.r_name AS region_name,
         MIN(ps.ps_supplycost) AS min_cost
  FROM partsupp ps
  JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  JOIN region r ON n.n_regionkey = r.r_regionkey
  JOIN part p ON p.p_partkey = ps.ps_partkey
  WHERE p.p_size BETWEEN 1 AND 50
    AND (p.p_type LIKE '%NICKEL' OR p.p_type LIKE '%BRASS'
         OR p.p_type LIKE '%STEEL' OR p.p_type LIKE '%COPPER'
         OR p.p_type LIKE '%TIN')
  GROUP BY ps.ps_partkey, r.r_name
),
candidates AS (
  SELECT
    p.p_size,
    CASE
      WHEN p.p_type LIKE '%NICKEL' THEN 'NICKEL'
      WHEN p.p_type LIKE '%BRASS' THEN 'BRASS'
      WHEN p.p_type LIKE '%STEEL' THEN 'STEEL'
      WHEN p.p_type LIKE '%COPPER' THEN 'COPPER'
      WHEN p.p_type LIKE '%TIN' THEN 'TIN'
    END AS type_pattern,
    r.r_name AS region_name,
    s.s_acctbal, s.s_name, n.n_name, p.p_partkey, p.p_mfgr,
    s.s_address, s.s_phone, s.s_comment,
    ROW_NUMBER() OVER (
      PARTITION BY p.p_size,
        CASE
          WHEN p.p_type LIKE '%NICKEL' THEN 'NICKEL'
          WHEN p.p_type LIKE '%BRASS' THEN 'BRASS'
          WHEN p.p_type LIKE '%STEEL' THEN 'STEEL'
          WHEN p.p_type LIKE '%COPPER' THEN 'COPPER'
          WHEN p.p_type LIKE '%TIN' THEN 'TIN'
        END,
        r.r_name
      ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey
    ) AS rn
  FROM part p
  JOIN partsupp ps ON p.p_partkey = ps.ps_partkey
  JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  JOIN region r ON n.n_regionkey = r.r_regionkey
  JOIN regional_min m
    ON m.ps_partkey = p.p_partkey
   AND m.region_name = r.r_name
   AND ps.ps_supplycost = m.min_cost
  WHERE p.p_size BETWEEN 1 AND 50
    AND (p.p_type LIKE '%NICKEL' OR p.p_type LIKE '%BRASS'
         OR p.p_type LIKE '%STEEL' OR p.p_type LIKE '%COPPER'
         OR p.p_type LIKE '%TIN')
)
SELECT
  p_size, type_pattern, region_name,
  s_acctbal, s_name, n_name, p_partkey, p_mfgr,
  s_address, s_phone, s_comment
FROM candidates
WHERE type_pattern IS NOT NULL AND rn <= 100;
