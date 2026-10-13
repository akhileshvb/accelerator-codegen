-- Answer grain: 16 `%WORD1%WORD2%` histograms (c_count → custdist).
CREATE MATERIALIZED VIEW accel_tpch_q13_matview AS
WITH patterns(comment_pattern) AS (
  VALUES
  ('%special%packages%'),
  ('%special%requests%'),
  ('%special%accounts%'),
  ('%special%deposits%'),
  ('%pending%packages%'),
  ('%pending%requests%'),
  ('%pending%accounts%'),
  ('%pending%deposits%'),
  ('%unusual%packages%'),
  ('%unusual%requests%'),
  ('%unusual%accounts%'),
  ('%unusual%deposits%'),
  ('%express%packages%'),
  ('%express%requests%'),
  ('%express%accounts%'),
  ('%express%deposits%')
),
per_cust AS (
  SELECT
    c.c_custkey,
    p.comment_pattern,
    COUNT(o.o_orderkey) FILTER (
      WHERE o.o_orderkey IS NOT NULL AND o.o_comment NOT LIKE p.comment_pattern
    ) AS c_count
  FROM customer c
  CROSS JOIN patterns p
  LEFT OUTER JOIN orders o ON c.c_custkey = o.o_custkey
  GROUP BY c.c_custkey, p.comment_pattern
)
SELECT comment_pattern, c_count, COUNT(*)::bigint AS custdist
FROM per_cust
GROUP BY comment_pattern, c_count;
