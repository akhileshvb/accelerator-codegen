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
  AND p_brand <> 'Brand#15'
  AND p_type NOT LIKE 'MEDIUM PLATED%'
  AND p_size IN (10, 34, 31, 2, 3, 15, 40, 28)
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

