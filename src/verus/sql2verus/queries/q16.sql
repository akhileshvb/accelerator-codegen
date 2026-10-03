-- TPC-H Q16 (catalog text), :brand as $1, :size1..:size8 as $2..$9. The type_pattern ('SMALL BRUSHED%') stays a literal: sql2verus takes only a literal LIKE pattern.
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey AND p_brand <> $1
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN ($2, $3, $4, $5, $6, $7, $8, $9)
  AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
