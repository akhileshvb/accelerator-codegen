CREATE OR REPLACE FUNCTION accel_tpch_q10_mv(start_date date)
RETURNS TABLE (
  c_custkey bigint,
  c_name character varying,
  revenue numeric,
  c_acctbal numeric,
  n_name character varying,
  c_address character varying,
  c_phone character varying,
  c_comment character varying
)
LANGUAGE sql STABLE AS $$
  SELECT
    m.c_custkey, m.c_name, m.revenue, m.c_acctbal, m.n_name,
    m.c_address, m.c_phone, m.c_comment
  FROM accel_tpch_q10_matview m
  WHERE m.start_date = start_date
  ORDER BY m.revenue DESC
$$;
