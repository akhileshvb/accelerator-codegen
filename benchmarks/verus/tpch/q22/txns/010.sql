INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer)
         + row_number() OVER (ORDER BY codes.code, balances.balance),
       'Q22 Campaign Account', source.c_address, source.c_nationkey,
       codes.code || '-555-0121', balances.balance,
       source.c_mktsegment, 'q22-batch'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source
CROSS JOIN (VALUES ('13'), ('31'), ('23'), ('29'), ('30'), ('18'), ('17')) AS codes(code)
CROSS JOIN (VALUES (8800.00), (9200.00)) AS balances(balance);
