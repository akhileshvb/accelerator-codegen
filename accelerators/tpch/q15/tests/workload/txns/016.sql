INSERT INTO supplier
SELECT * REPLACE ((SELECT MAX(s_suppkey) + 1 FROM supplier) AS s_suppkey,
  'New Distribution Co' AS s_name, '15 Warehouse Road' AS s_address,
  '11-222-333-4444' AS s_phone, 'q15-new-supplier' AS s_comment)
FROM supplier ORDER BY s_suppkey LIMIT 1;
INSERT INTO orders
SELECT * REPLACE (
  (SELECT GREATEST(MAX(o_orderkey), (SELECT MAX(l_orderkey) FROM lineitem)) + 1 FROM orders) AS o_orderkey,
  DATE '1996-02-01' AS o_orderdate, 'F' AS o_orderstatus,
  150000.00 AS o_totalprice, 'q15-new-supplier-order' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT * REPLACE ((SELECT o_orderkey FROM orders WHERE o_comment = 'q15-new-supplier-order') AS l_orderkey,
  (SELECT s_suppkey FROM supplier WHERE s_comment = 'q15-new-supplier') AS l_suppkey,
  150000.00 AS l_extendedprice, 0.00 AS l_discount, DATE '1996-02-20' AS l_shipdate,
  DATE '1996-02-21' AS l_commitdate, DATE '1996-02-23' AS l_receiptdate,
  'q15-new-supplier-line' AS l_comment)
FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
