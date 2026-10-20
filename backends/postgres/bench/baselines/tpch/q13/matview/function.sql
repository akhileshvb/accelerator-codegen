CREATE OR REPLACE FUNCTION accel_tpch_q13_mv(comment_pattern text)
RETURNS TABLE (c_count bigint, custdist bigint)
LANGUAGE sql STABLE AS $$
  SELECT m.c_count, m.custdist
  FROM accel_tpch_q13_matview m
  WHERE m.comment_pattern = comment_pattern
  ORDER BY m.custdist DESC, m.c_count DESC
$$;
