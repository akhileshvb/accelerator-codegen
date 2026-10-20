SELECT SUM(revenue) AS revenue
FROM accel_tpch_q19_immv
WHERE (
  p_brand = 'Brand#31'
  AND cont_class = 'SM'
  AND l_quantity >= 4 AND l_quantity <= 4 + 10
  AND p_size BETWEEN 1 AND 5
) OR (
  p_brand = 'Brand#12'
  AND cont_class = 'MED'
  AND l_quantity >= 15 AND l_quantity <= 15 + 10
  AND p_size BETWEEN 1 AND 10
) OR (
  p_brand = 'Brand#51'
  AND cont_class = 'LG'
  AND l_quantity >= 23 AND l_quantity <= 23 + 10
  AND p_size BETWEEN 1 AND 15
)
