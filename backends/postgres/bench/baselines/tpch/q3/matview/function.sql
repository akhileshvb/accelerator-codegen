CREATE OR REPLACE FUNCTION accel_tpch_q3_mv(market_segment text, cutoff_date date)
RETURNS TABLE (
  l_orderkey bigint,
  revenue numeric,
  o_orderdate date,
  o_shippriority integer
)
LANGUAGE sql
STABLE
AS $$
  SELECT m.l_orderkey, m.revenue, m.o_orderdate, m.o_shippriority
  FROM accel_tpch_q3_matview m
  WHERE m.c_mktsegment = market_segment
    AND m.cutoff_date = cutoff_date
  ORDER BY m.revenue DESC, m.o_orderdate
$$;
