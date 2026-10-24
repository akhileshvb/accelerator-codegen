INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer)
         + row_number() OVER (ORDER BY codes.code, balances.balance),
       'Q22 Regional Account', source.c_address, source.c_nationkey,
       codes.code || '-555-0124', balances.balance,
       source.c_mktsegment, 'q22-large'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source
CROSS JOIN (SELECT DISTINCT substring(c_phone, 1, 2) AS code
            FROM customer WHERE c_comment = 'q22-seed') AS codes
CROSS JOIN (VALUES (8800.00), (9200.00)) AS balances(balance);
