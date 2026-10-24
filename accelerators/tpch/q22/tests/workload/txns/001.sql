INSERT INTO customer
SELECT (SELECT max(c_custkey) + 1 FROM customer), 'Q22 New Account',
       c_address, c_nationkey, '13-555-0120', 9800.00,
       c_mktsegment, 'q22-onboarding'
FROM customer ORDER BY c_custkey LIMIT 1;
