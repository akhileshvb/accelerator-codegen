CREATE OR REPLACE FUNCTION accel_tpch_q9_mv(color text)
RETURNS TABLE (nation character varying, o_year integer, sum_profit numeric)
LANGUAGE sql STABLE AS $$
  SELECT m.nation, m.o_year, SUM(m.amount)
  FROM accel_tpch_q9_matview m
  WHERE m.color = color
  GROUP BY m.nation, m.o_year
  ORDER BY m.nation, m.o_year DESC
$$;
