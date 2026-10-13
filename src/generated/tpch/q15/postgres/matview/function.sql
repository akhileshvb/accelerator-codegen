CREATE OR REPLACE FUNCTION accel_tpch_q15_mv(start_date date)
RETURNS TABLE (
  s_suppkey bigint,
  s_name character varying,
  s_address character varying,
  s_phone character varying,
  total_revenue numeric
)
LANGUAGE sql STABLE AS $$
  SELECT m.s_suppkey, m.s_name, m.s_address, m.s_phone, m.total_revenue
  FROM accel_tpch_q15_matview m
  WHERE m.start_date = start_date
  ORDER BY m.s_suppkey
$$;
