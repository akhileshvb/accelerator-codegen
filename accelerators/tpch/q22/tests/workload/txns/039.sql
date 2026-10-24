INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer)
         + row_number() OVER (ORDER BY fixtures.ordinal),
       'Q22 Extreme Account', source.c_address, source.c_nationkey,
       'E1-555-0198', fixtures.balance, source.c_mktsegment, 'q22-extreme'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source
CROSS JOIN (VALUES
  (1, 9999999999999.99), (2, 9999999999999.99), (3, 9999999999999.99),
  (4, 0.01), (5, -9999999999999.99), (6, 0.00)
) AS fixtures(ordinal, balance);
