SELECT cntrycode, numcust, totacctbal
FROM accel_tpch_q22_matview
WHERE codeset = 'seed1'
ORDER BY cntrycode
