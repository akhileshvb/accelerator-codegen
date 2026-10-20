SELECT
  ps_partkey,
  SUM(ps_supplycost * ps_availqty) AS value
FROM
  partsupp,
  supplier,
  nation
WHERE
  ps_suppkey = s_suppkey
  AND s_nationkey = n_nationkey
  AND n_name = 'INDIA'
GROUP BY
  ps_partkey
HAVING
  SUM(ps_supplycost * ps_availqty) > (
    SELECT
      SUM(ps_supplycost * ps_availqty) * 1
    FROM
      partsupp,
      supplier,
      nation
    WHERE
      ps_suppkey = s_suppkey
      AND s_nationkey = n_nationkey
      AND n_name = 'INDIA')
ORDER BY
  value DESC;

