-- TPC-H Q16 (catalog text), :brand as $1, :type_pattern (:type || '%', e.g. 'SMALL BRUSHED%') as $2, :size1..:size8 as $3..$10. The s_comment pattern '%Customer%Complaints%' is fixed by the TPC-H spec (not a substitution parameter) and stays a literal.
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey AND p_brand <> $1
  AND p_type NOT LIKE $2
  AND p_size IN ($3, $4, $5, $6, $7, $8, $9, $10)
  AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
