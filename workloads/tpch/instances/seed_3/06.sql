SELECT
  SUM(l_extendedprice * l_discount) AS revenue
FROM
  lineitem
WHERE
  l_shipdate >= DATE '1994-01-01'
  AND l_shipdate < DATE '1994-01-01' + interval '1' year
  AND l_discount BETWEEN 0.09 - 0.01 AND 0.09 + 0.01
  AND l_quantity < 25;

