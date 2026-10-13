CREATE OR REPLACE FUNCTION accel_tpch_q17_mv(brand text, container text)
RETURNS TABLE (avg_yearly numeric)
LANGUAGE sql STABLE AS $$
  SELECT SUM(m.sum_price) / 7.0
  FROM accel_tpch_q17_matview m
  WHERE m.p_brand = brand
    AND m.p_container = container
$$;
