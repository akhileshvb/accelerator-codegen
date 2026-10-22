CREATE OR REPLACE FUNCTION accel_tpch_q19_mv(
  brand1 text, quantity1 numeric,
  brand2 text, quantity2 numeric,
  brand3 text, quantity3 numeric
)
RETURNS TABLE (revenue numeric)
LANGUAGE sql STABLE AS $$
  SELECT SUM(m.revenue)
  FROM accel_tpch_q19_matview m
  WHERE (
    m.p_brand = brand1
    AND m.cont_class = 'SM'
    AND m.l_quantity >= quantity1 AND m.l_quantity <= quantity1 + 10
    AND m.p_size BETWEEN 1 AND 5
  ) OR (
    m.p_brand = brand2
    AND m.cont_class = 'MED'
    AND m.l_quantity >= quantity2 AND m.l_quantity <= quantity2 + 10
    AND m.p_size BETWEEN 1 AND 10
  ) OR (
    m.p_brand = brand3
    AND m.cont_class = 'LG'
    AND m.l_quantity >= quantity3 AND m.l_quantity <= quantity3 + 10
    AND m.p_size BETWEEN 1 AND 15
  )
$$;
