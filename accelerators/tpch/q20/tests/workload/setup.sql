INSERT INTO supplier
SELECT (SELECT max(s_suppkey) FROM supplier) + CAST(row_number() OVER (ORDER BY n_nationkey, branch) AS INTEGER),
       'Q20 ' || n_name || ' ' || branch, 'Warehouse ' || n_name || ' ' || branch,
       n_nationkey, '10-100-100-1000', 1000.00, 'q20-base'
FROM nation CROSS JOIN (VALUES (1), (2)) branches(branch);
INSERT INTO part
SELECT (SELECT max(p_partkey) FROM part) + CAST(row_number() OVER (ORDER BY s_suppkey) AS INTEGER),
       'gainsboro test ' || s_suppkey, 'Manufacturer#1', 'Brand#11', 'STANDARD', 12,
       'SM BOX', 100.00, 'q20-base'
FROM supplier WHERE s_comment = 'q20-base';
INSERT INTO partsupp
SELECT p_partkey, s_suppkey, 21, 10.00, 'q20-base'
FROM part JOIN supplier ON p_name = 'gainsboro test ' || s_suppkey
WHERE p_comment = 'q20-base' AND s_comment = 'q20-base';
INSERT INTO orders
SELECT (SELECT max(o_orderkey) FROM orders) + CAST(row_number() OVER (ORDER BY s_suppkey) AS INTEGER),
       (SELECT min(c_custkey) FROM customer), 'F', 1000.00, DATE '1994-01-01',
       '3-MEDIUM', 'Clerk#000000001', 0, 'q20-order/' || s_suppkey
FROM supplier WHERE s_comment = 'q20-base';
INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, line_no, quantity, 100.00, 0.00, 0.00,
       'N', 'F', shipdate, shipdate, shipdate + 2, 'DELIVER IN PERSON', 'AIR', label
FROM partsupp JOIN orders ON o_comment = 'q20-order/' || ps_suppkey
CROSS JOIN (VALUES
 (1, 10.00, DATE '1995-01-01', 'q20-base-A'),
 (2, 14.00, DATE '1995-06-15', 'q20-base-B'),
 (3, 16.00, DATE '1995-12-31', 'q20-base-C'),
 (4, 10.00, DATE '1994-06-15', 'q20-base-D'),
 (5, 12.00, DATE '1996-06-15', 'q20-base-E')) shipments(line_no, quantity, shipdate, label)
WHERE ps_comment = 'q20-base';
