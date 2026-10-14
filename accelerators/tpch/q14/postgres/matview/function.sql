CREATE OR REPLACE FUNCTION accel_tpch_q14_mv(start_date date)
RETURNS TABLE (promo_revenue numeric)
LANGUAGE sql STABLE AS $$
  SELECT 100.00 * SUM(m.promo_rev) / SUM(m.all_rev)
  FROM accel_tpch_q14_matview m
  WHERE m.l_shipdate >= start_date
    AND m.l_shipdate < start_date + INTERVAL '1' month
$$;
