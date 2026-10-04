INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + CAST(row_number() OVER (ORDER BY s_suppkey) AS INTEGER),
       'gainsboro alternate ' || s_suppkey, 'Manufacturer#2', 'Brand#22', 'STANDARD',
       15, 'SM BOX', 110.00, 'q20-alternate'
FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
INSERT INTO lineitem
SELECT o_orderkey, p_partkey, s_suppkey, 6, 20.00, 110.00, 0.00, 0.00,
       'N', 'F', DATE '1995-06-15', DATE '1995-06-16', DATE '1995-06-17',
       'DELIVER IN PERSON', 'AIR', 'q20-alternate'
FROM supplier JOIN part ON p_name = 'gainsboro alternate ' || s_suppkey
JOIN orders ON o_comment = 'q20-order/' || s_suppkey
WHERE p_comment = 'q20-alternate';
INSERT INTO partsupp
SELECT p_partkey, s_suppkey, 11, 11.00, 'q20-alternate'
FROM supplier JOIN part ON p_name = 'gainsboro alternate ' || s_suppkey
WHERE p_comment = 'q20-alternate';
