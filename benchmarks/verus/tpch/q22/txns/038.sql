UPDATE customer
SET c_acctbal = CASE c_name WHEN 'Boundary low' THEN 100.00
                          WHEN 'Boundary equal' THEN 200.00 ELSE 300.00 END
WHERE c_comment = 'q22-boundary';
UPDATE customer SET c_acctbal = 0.01
WHERE c_name = 'Fraction low' AND c_comment = 'q22-fraction';
UPDATE customer SET c_acctbal = -0.01
WHERE c_name = 'Nonpositive negative' AND c_comment = 'q22-zero';
INSERT INTO customer
SELECT greatest((SELECT max(c_custkey) FROM customer),
                (SELECT max(o_custkey) FROM orders)) + 1, 'Positive cent',
       c_address, c_nationkey, 'N0-555-0102', 0.02,
       c_mktsegment, 'q22-zero'
FROM customer ORDER BY c_custkey LIMIT 1;
