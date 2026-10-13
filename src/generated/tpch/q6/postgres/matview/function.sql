CREATE OR REPLACE FUNCTION accel_tpch_q6_mv(start_date date, discount numeric, quantity numeric)
RETURNS TABLE (revenue numeric)
LANGUAGE sql STABLE AS $$
  SELECT SUM(m.revenue)
  FROM accel_tpch_q6_matview m
  WHERE m.l_shipdate >= start_date
    AND m.l_shipdate < start_date + INTERVAL '1' year
    AND m.l_discount BETWEEN discount - 0.01 AND discount + 0.01
    AND m.l_quantity < quantity
$$;
