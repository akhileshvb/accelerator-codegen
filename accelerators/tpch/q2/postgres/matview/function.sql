CREATE OR REPLACE FUNCTION accel_tpch_q2_mv(part_size integer, part_type_pattern text, region_name text)
RETURNS TABLE (
  s_acctbal numeric,
  s_name character varying,
  n_name character varying,
  p_partkey bigint,
  p_mfgr character varying,
  s_address character varying,
  s_phone character varying,
  s_comment character varying
)
LANGUAGE sql
STABLE
AS $$
  SELECT
    m.s_acctbal, m.s_name, m.n_name, m.p_partkey, m.p_mfgr,
    m.s_address, m.s_phone, m.s_comment
  FROM accel_tpch_q2_matview m
  WHERE m.p_size = part_size
    AND m.type_pattern = regexp_replace(part_type_pattern, '^%', '')
    AND m.region_name = region_name
  ORDER BY m.s_acctbal DESC, m.n_name, m.s_name, m.p_partkey
$$;
