CREATE OR REPLACE FUNCTION accel_tpch_q20_mv(start_date date, part_name_pattern text, nation_name text)
RETURNS TABLE (s_name character varying, s_address character varying)
LANGUAGE sql STABLE AS $$
  SELECT m.s_name, m.s_address
  FROM accel_tpch_q20_matview m
  WHERE m.color = regexp_replace(part_name_pattern, '%$', '')
    AND m.year = EXTRACT(year FROM start_date)::integer
    AND m.nation = nation_name
  ORDER BY m.s_name
$$;
