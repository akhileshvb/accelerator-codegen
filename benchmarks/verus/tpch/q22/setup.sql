INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer)
         + row_number() OVER (ORDER BY source.code, balances.balance),
       'Q22 Prospect', source.c_address, source.c_nationkey,
       source.code || '-555-0100', balances.balance,
       source.c_mktsegment, 'q22-seed'
FROM (
  SELECT *, substring(c_phone, 1, 2) AS code,
         row_number() OVER (PARTITION BY substring(c_phone, 1, 2) ORDER BY c_custkey) AS ordinal
  FROM customer
) AS source
CROSS JOIN (VALUES (1250.00), (4500.00), (5500.00), (7000.00), (8500.00), (9500.00)) AS balances(balance)
WHERE source.ordinal = 1;

INSERT INTO customer
SELECT (SELECT max(c_custkey) FROM customer)
         + row_number() OVER (ORDER BY fixtures.name),
       fixtures.name, source.c_address, source.c_nationkey,
       fixtures.phone, fixtures.balance, source.c_mktsegment, fixtures.marker
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source
CROSS JOIN (VALUES
  ('Boundary low', 'B1-555-0100', 100.00, 'q22-boundary'),
  ('Boundary equal', 'B1-555-0101', 200.00, 'q22-boundary'),
  ('Boundary high', 'B1-555-0102', 300.00, 'q22-boundary'),
  ('Fraction low', 'B2-555-0100', 0.01, 'q22-fraction'),
  ('Fraction high one', 'B2-555-0101', 0.02, 'q22-fraction'),
  ('Fraction high two', 'B2-555-0102', 0.02, 'q22-fraction'),
  ('Nonpositive zero', 'N0-555-0100', 0.00, 'q22-zero'),
  ('Nonpositive negative', 'N0-555-0101', -1.00, 'q22-zero'),
  ('Upper low', 'AA-555-0100', 100.00, 'q22-text'),
  ('Upper high', 'AA-555-0101', 900.00, 'q22-text'),
  ('Lower low', 'aa-555-0100', 100.00, 'q22-text'),
  ('Lower high', 'aa-555-0101', 900.00, 'q22-text'),
  ('Trailing low', 'A -555-0100', 100.00, 'q22-text'),
  ('Trailing high', 'A -555-0101', 900.00, 'q22-text'),
  ('Spaces low', '  -555-0100', 100.00, 'q22-text'),
  ('Spaces high', '  -555-0101', 900.00, 'q22-text'),
  ('Empty low', '', 100.00, 'q22-text'),
  ('Empty high', '', 900.00, 'q22-text'),
  ('Short low', 'x', 100.00, 'q22-text'),
  ('Short high', 'x', 900.00, 'q22-text')
) AS fixtures(name, phone, balance, marker);
