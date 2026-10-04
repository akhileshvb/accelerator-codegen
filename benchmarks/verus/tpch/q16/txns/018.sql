UPDATE part SET p_brand = 'Brand#45', p_size = 14
WHERE p_partkey IN (
    SELECT DISTINCT p.p_partkey FROM part p JOIN partsupp ps ON p.p_partkey = ps.ps_partkey
    WHERE p.p_name NOT LIKE 'Verus %' AND p.p_brand <> 'Brand#45'
      AND p.p_type NOT LIKE 'SMALL BRUSHED%'
      AND ps.ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
    ORDER BY p.p_partkey LIMIT 20
);
