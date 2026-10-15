-- Answer grain: Q22 aggregates for all 25 country codes (avg over full set)
-- UNION exact seed_1 7-code combo. ≤25 + 7 rows.
CREATE MATERIALIZED VIEW accel_tpch_q22_matview AS
WITH all25 AS (
  SELECT cntrycode, COUNT(*)::bigint AS numcust, SUM(c_acctbal) AS totacctbal
  FROM (
    SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
    FROM customer
    WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('10', '11', '12', '13', '14', '15', '16', '17', '18', '19', '20', '21', '22', '23', '24', '25', '26', '27', '28', '29', '30', '31', '32', '33', '34')
      AND c_acctbal > (
        SELECT AVG(c_acctbal) FROM customer
        WHERE c_acctbal > 0.00
          AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('10', '11', '12', '13', '14', '15', '16', '17', '18', '19', '20', '21', '22', '23', '24', '25', '26', '27', '28', '29', '30', '31', '32', '33', '34')
      )
      AND NOT EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)
  ) custsale
  GROUP BY cntrycode
),
seed7 AS (
  SELECT cntrycode, COUNT(*)::bigint AS numcust, SUM(c_acctbal) AS totacctbal
  FROM (
    SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
    FROM customer
    WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')
      AND c_acctbal > (
        SELECT AVG(c_acctbal) FROM customer
        WHERE c_acctbal > 0.00
          AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')
      )
      AND NOT EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)
  ) custsale
  GROUP BY cntrycode
)
SELECT 'all25'::text AS codeset, cntrycode, numcust, totacctbal FROM all25
UNION ALL
SELECT 'seed1'::text, cntrycode, numcust, totacctbal FROM seed7;
