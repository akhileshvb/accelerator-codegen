UPDATE part
SET p_type = CASE WHEN p_type LIKE 'PROMO%' THEN 'STANDARD ANODIZED TIN'
                  ELSE 'PROMO ANODIZED TIN' END
WHERE p_partkey IN (
    SELECT DISTINCT l_partkey FROM lineitem
    WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
      AND l_comment NOT LIKE 'v14-%'
    ORDER BY l_partkey LIMIT 6
);
