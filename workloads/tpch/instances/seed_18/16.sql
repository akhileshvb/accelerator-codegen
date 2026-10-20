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
  AND p_brand <> 'Brand#32'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (40, 3, 33, 36, 4, 13, 35, 9)
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

