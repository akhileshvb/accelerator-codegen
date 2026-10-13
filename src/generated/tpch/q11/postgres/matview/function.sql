CREATE OR REPLACE FUNCTION accel_tpch_q11_mv(nation_name text, fraction numeric)
RETURNS TABLE (ps_partkey bigint, value numeric)
LANGUAGE sql STABLE AS $$
  SELECT m.ps_partkey, m.value
  FROM accel_tpch_q11_matview m
  WHERE m.n_name = nation_name
    AND m.value > m.nation_total * fraction
  ORDER BY m.value DESC
$$;
