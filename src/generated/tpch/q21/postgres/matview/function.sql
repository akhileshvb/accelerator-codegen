CREATE OR REPLACE FUNCTION accel_tpch_q21_mv(nation_name text)
RETURNS TABLE (s_name character varying, numwait bigint)
LANGUAGE sql STABLE AS $$
  SELECT m.s_name, m.numwait
  FROM accel_tpch_q21_matview m
  WHERE m.n_name = nation_name
  ORDER BY m.numwait DESC, m.s_name
$$;
