SELECT
  p_brand,
  p_type,
  p_size,
  COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM
  partsupp,
  part
WHERE
  p_partkey = ps_partkey
  AND p_brand <> 'Brand#31'
  AND p_type NOT LIKE 'MEDIUM BRUSHED%'
  AND p_size IN (34, 6, 37, 29, 33, 47, 30, 24)
  AND ps_suppkey NOT IN (
    SELECT
      s_suppkey
    FROM
      supplier
    WHERE
      s_comment LIKE '%Customer%Complaints%')
GROUP BY
  p_brand,
  p_type,
  p_size
ORDER BY
  supplier_cnt DESC,
  p_brand,
  p_type,
  p_size;

