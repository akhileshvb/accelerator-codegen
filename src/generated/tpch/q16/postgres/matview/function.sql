CREATE OR REPLACE FUNCTION accel_tpch_q16_mv(
  brand text, type_pattern text,
  size1 integer, size2 integer, size3 integer, size4 integer,
  size5 integer, size6 integer, size7 integer, size8 integer
)
RETURNS TABLE (
  p_brand character varying,
  p_type character varying,
  p_size integer,
  supplier_cnt bigint
)
LANGUAGE sql STABLE AS $$
  SELECT m.p_brand, m.p_type, m.p_size, m.supplier_cnt
  FROM accel_tpch_q16_matview m
  WHERE m.p_brand <> brand
    AND m.p_type NOT LIKE type_pattern
    AND m.p_size IN (size1, size2, size3, size4, size5, size6, size7, size8)
  ORDER BY m.supplier_cnt DESC, m.p_brand, m.p_type, m.p_size
$$;
