UPDATE part SET p_size = CASE WHEN p_size = 50 THEN 1 ELSE p_size + 1 END,
                p_brand = 'Brand#14'
WHERE p_partkey IN (
    SELECT DISTINCT p.p_partkey FROM part p JOIN partsupp ps ON p.p_partkey = ps.ps_partkey
    WHERE p.p_name NOT LIKE 'Verus %' AND p.p_type NOT LIKE 'SMALL BRUSHED%'
      AND ps.ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
    ORDER BY p.p_partkey DESC LIMIT 128
);
