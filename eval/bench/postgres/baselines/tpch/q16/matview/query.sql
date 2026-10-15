SELECT p_brand, p_type, p_size, supplier_cnt
FROM accel_tpch_q16_matview
WHERE p_brand <> 'Brand#13'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
