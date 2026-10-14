-- TPC-H Q22 (catalog text), :country_code1..:country_code7 as $1..$7.
WITH custsale AS (
  SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
  FROM customer
  WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ($1, $2, $3, $4, $5, $6, $7)
    AND c_acctbal > (SELECT AVG(c_acctbal) FROM customer WHERE c_acctbal > 0.00
                     AND SUBSTRING(c_phone FROM 1 FOR 2) IN ($1, $2, $3, $4, $5, $6, $7))
    AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey))
SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
FROM custsale
GROUP BY cntrycode
ORDER BY cntrycode
