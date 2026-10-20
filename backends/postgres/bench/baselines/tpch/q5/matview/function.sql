CREATE OR REPLACE FUNCTION accel_tpch_q5_mv(region_name text, start_date date)
RETURNS TABLE (n_name character varying, revenue numeric)
LANGUAGE sql STABLE AS $$
  SELECT m.n_name, SUM(m.revenue)
  FROM accel_tpch_q5_matview m
  WHERE m.r_name = region_name
    AND m.o_orderdate >= start_date
    AND m.o_orderdate < start_date + INTERVAL '1' year
  GROUP BY m.n_name
  ORDER BY SUM(m.revenue) DESC
$$;
