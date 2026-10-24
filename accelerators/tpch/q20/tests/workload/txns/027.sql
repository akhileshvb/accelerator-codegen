INSERT INTO supplier
SELECT max(s_suppkey) + 1, 'Q20 New Canadian Vendor', 'Toronto distribution center',
       (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA'), '10-100-100-1001', 2000.00, 'q20-new' FROM supplier;
INSERT INTO part
SELECT max(p_partkey) + 1, 'gainsboro new product', 'Manufacturer#1', 'Brand#11',
       'STANDARD', 12, 'SM BOX', 100.00, 'q20-new' FROM part;
INSERT INTO partsupp
SELECT p_partkey, s_suppkey, 21, 10.00, 'q20-new' FROM part CROSS JOIN supplier
WHERE p_comment = 'q20-new' AND s_comment = 'q20-new';
INSERT INTO orders
SELECT max(o_orderkey) + 1, (SELECT min(c_custkey) FROM customer), 'O', 100.00,
       DATE '1995-06-01', '3-MEDIUM', 'Clerk#000000001', 0, 'q20-new' FROM orders;
INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, 1, 40.00, 100.00, 0.00, 0.00,
       'N', 'O', DATE '1995-06-15', DATE '1995-06-16', DATE '1995-06-17',
       'DELIVER IN PERSON', 'AIR', 'q20-new'
FROM orders CROSS JOIN partsupp WHERE o_comment = 'q20-new' AND ps_comment = 'q20-new';
