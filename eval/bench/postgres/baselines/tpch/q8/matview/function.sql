CREATE OR REPLACE FUNCTION accel_tpch_q8_mv(nation_name text, region_name text, part_type text)
RETURNS TABLE (o_year integer, mkt_share numeric)
LANGUAGE sql STABLE AS $$
  SELECT
    m.o_year,
    SUM(CASE WHEN m.nation = nation_name THEN m.volume ELSE 0 END) / SUM(m.volume)
  FROM accel_tpch_q8_matview m
  WHERE m.r_name = region_name AND m.p_type = part_type
  GROUP BY m.o_year
  ORDER BY m.o_year
$$;
