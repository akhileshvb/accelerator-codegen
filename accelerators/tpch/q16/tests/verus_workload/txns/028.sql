INSERT INTO partsupp
SELECT p.p_partkey, s.s_suppkey, 200, 47.00, 'v16 bulk sourcing'
FROM (
    SELECT DISTINCT p.p_partkey FROM part p JOIN partsupp ps ON p.p_partkey = ps.ps_partkey
    WHERE p.p_name NOT LIKE 'Verus %' AND p.p_type NOT LIKE 'SMALL BRUSHED%'
      AND ps.ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
    ORDER BY p.p_partkey LIMIT 64
) p CROSS JOIN supplier s
WHERE s.s_name = 'Supplier#V16-09'
  AND NOT EXISTS (SELECT 1 FROM partsupp ps WHERE ps.ps_partkey = p.p_partkey AND ps.ps_suppkey = s.s_suppkey);
